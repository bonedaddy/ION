/*
	brsscla.c:	BP Bundle Relay Service convergence-layer
			server daemon.  Handles both transmission
			and reception.

	Author: Scott Burleigh, JPL

	Copyright (c) 2006, California Institute of Technology.
	ALL RIGHTS RESERVED.  U.S. Government Sponsorship
	acknowledged.
	
									*/
#include "brscla.h"

static pthread_t	brssclaMainThread(pthread_t tid)
{
	static pthread_t	mainThread = 0;

	if (tid)
	{
		mainThread = tid;
	}

	return mainThread;
}

static void	interruptThread()
{
	pthread_t	mainThread = brssclaMainThread(0);

	isignal(SIGTERM, interruptThread);
	if (mainThread != pthread_self())
	{
		pthread_kill(mainThread, SIGTERM);
	}
}

/*	*	*	Sender thread functions	*	*	*	*/

typedef struct
{
	VOutduct	*vduct;
	int		baseDuctNbr;
	int		lastDuctNbr;
	int		*brsSockets;
} SenderThreadParms;

static void	*terminateSenderThread(SenderThreadParms *parms)
{
	writeErrmsgMemos();
	writeMemo("[i] sender thread stopping.");
	return NULL;
}

static void	*sendBundles(void *parm)
{
	/*	Main loop for single bundle transmission thread
	 *	serving all BRS sockets.				*/

	SenderThreadParms	*parms = (SenderThreadParms *) parm;
	unsigned char		*buffer;
	Outduct			outduct;
	Sdr			sdr;
	Outflow			outflows[3];
	int			i;
	Object			bundleZco;
	BpExtendedCOS		extendedCOS;
	char			destDuctName[MAX_CL_DUCT_NAME_LEN + 1];
	unsigned int		bundleLength;
	int			ductNbr;
	int			bytesSent;
	int			failedTransmissions = 0;

	buffer = MTAKE(TCPCLA_BUFSZ);
	if (buffer == NULL)
	{
		putErrmsg("No memory for TCP buffer in brsscla.", NULL);
		return terminateSenderThread(parms);
	}

	sdr = getIonsdr();
	sdr_read(sdr, (char *) &outduct, sdr_list_data(sdr,
			parms->vduct->outductElt), sizeof(Outduct));
	memset((char *) outflows, 0, sizeof outflows);
	outflows[0].outboundBundles = outduct.bulkQueue;
	outflows[1].outboundBundles = outduct.stdQueue;
	outflows[2].outboundBundles = outduct.urgentQueue;
	for (i = 0; i < 3; i++)
	{
		outflows[i].svcFactor = 1 << i;
	}

	/*	Can now begin transmitting to clients.			*/

	iblock(SIGTERM);
	while (!(sm_SemEnded(parms->vduct->semaphore)))
	{
		if (bpDequeue(parms->vduct, outflows, &bundleZco,
				&extendedCOS, destDuctName) < 0)
		{
			break;
		}

		if (bundleZco == 0)		/*	Interrupted.	*/
		{
			continue;
		}

		bundleLength = zco_length(sdr, bundleZco);
		ductNbr = atoi(destDuctName);
		if (ductNbr >= parms->baseDuctNbr
		&& ductNbr <= parms->lastDuctNbr
		&& parms->brsSockets[(i = ductNbr - parms->baseDuctNbr)] != -1)
		{
			bytesSent = sendBundleByTCP(NULL, parms->brsSockets + i,
					bundleLength, bundleZco, buffer);
			if (bytesSent < 0)
			{
				putErrmsg("Can't send bundle.", NULL);
				break;
			}

			if (bytesSent < bundleLength)
			{
				failedTransmissions++;
			}
		}
		else	/*	Can't send it; just discard it.		*/
		{
			sdr_begin_xn(sdr);
			zco_destroy_reference(sdr, bundleZco);
			if (sdr_end_xn(sdr) < 0)
			{
				putErrmsg("Can't destroy ZCO reference.", NULL);
				break;
			}
		}

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}

	pthread_kill(brssclaMainThread(0), SIGTERM);
	writeMemoNote("[i] brsscla outduct has ended",
			itoa(failedTransmissions));
	MRELEASE(buffer);
	return terminateSenderThread(parms);
}

/*	*	*	Receiver thread functions	*	*	*/

static int	reforwardStrandedBundles(int nodeNbr)
{
	Sdr	sdr = getIonsdr();
	BpDB	*bpConstants = getBpConstants();
	Object	elt;
	Object	eventObj;
		OBJ_POINTER(BpEvent, event);
		OBJ_POINTER(Bundle, bundle);

	sdr_begin_xn(sdr);
	for (elt = sdr_list_first(sdr, bpConstants->timeline); elt;
			elt = sdr_list_next(sdr, elt))
	{
		eventObj = sdr_list_data(sdr, elt);
		GET_OBJ_POINTER(sdr, BpEvent, event, eventObj);
		if (event->type != expiredTTL)
		{
			continue;
		}

		/*	Have got a bundle that still exists.		*/

		GET_OBJ_POINTER(sdr, Bundle, bundle, event->ref);
		if (bundle->dlvQueueElt || bundle->fragmentElt
		|| bundle->fwdQueueElt || bundle->overdueElt
		|| bundle->ctDueElt || bundle->xmitsNeeded > 0)
		{
			/*	No need to reforward.			*/

			continue;
		}

		/*	A stranded bundle, awaiting custody acceptance.
		 *	Might be for a neighbor other than the one
		 *	that just reconnected, but in that case the
		 *	bundle will be reforwarded and requeued and
		 *	go into the bit bucket again; no harm.  Note,
		 *	though, that this means that a BRS server
		 *	would be a bad candidate for gateway into a
		 *	space subnet: due to long OWLTs, there might
		 *	be a lot of bundles sent via LTP that are
		 *	awaiting custody acceptance, so reconnection
		 *	of a BRS client might trigger retransmission
		 *	of a lot of bundles on the space link in
		 *	addition to the ones to be issued via brss.	*/

		if (bpReforwardBundle(event->ref) < 0)
		{
			sdr_cancel_xn(sdr);
			putErrmsg("brss reforward failed.", NULL);
			return -1;
		}
	}

	if (sdr_end_xn(sdr) < 0)
	{
		putErrmsg("brss reforwarding failed.", NULL);
		return -1;
	}

	return 0;
}

typedef struct
{
	char		senderEidBuffer[SDRSTRING_BUFSZ];
	char		*senderEid;
	VInduct		*vduct;
	LystElt		elt;
	pthread_mutex_t	*mutex;
	pthread_t	thread;
	int		bundleSocket;
	unsigned long	ductNbr;
	int		*authenticated;
	int		baseDuctNbr;
	int		lastDuctNbr;
	int		*brsSockets;
} ReceiverThreadParms;

static void	terminateReceiverThread(ReceiverThreadParms *parms)
{
	int	senderSocket;

	writeErrmsgMemos();
	writeMemo("[i] brsscla receiver thread stopping.");
	pthread_mutex_lock(parms->mutex);
	if (parms->bundleSocket != -1)
	{
		close(parms->bundleSocket);
		if (parms->ductNbr != (unsigned long) -1)
		{
			senderSocket = parms->ductNbr - parms->baseDuctNbr;
			if (parms->brsSockets[senderSocket] ==
					parms->bundleSocket)
			{
				/*	Stop sender thread transmission
				 *	over this socket.		*/

				parms->brsSockets[senderSocket] = -1;
			}
		}

		parms->bundleSocket = -1;
	}

	lyst_delete(parms->elt);
	pthread_mutex_unlock(parms->mutex);
}

static void	*receiveBundles(void *parm)
{
	/*	Main loop for bundle reception thread on one
	 *	connection, terminating when connection is lost.	*/

	ReceiverThreadParms	*parms = (ReceiverThreadParms *) parm;
	time_t			currentTime;
	unsigned char		sdnvText[10];
	int			sdnvLength = 0;
	unsigned long		ductNbr;
	int			senderSocket;
	char			registration[24];
	time_t			timeTag;
	char			keyName[32];
	char			key[DIGEST_LEN];
	int			keyBufLen = sizeof key;
	int			keyLen;
	char			errtext[300];
	char			digest[DIGEST_LEN];
	int			threadRunning = 1;
	AcqWorkArea		*work;
	char			*buffer;

	currentTime = time(NULL);
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	/*	Get duct number, expressed as an SDNV.			*/

	while (1)
	{
		switch (receiveBytesByTCP(parms->bundleSocket,
				(char *) (sdnvText + sdnvLength), 1))
		{
			case 1:
				break;		/*	Out of switch.	*/
	
			case -1:
				putErrmsg("Can't get duct number.", NULL);
	
				/*	Intentional fall-through.	*/
	
			default:		/*	Inauthentic.	*/
				*parms->authenticated = 1;
				terminateReceiverThread(parms);
				MRELEASE(parms);
				return NULL;
		}

		if ((*(sdnvText + sdnvLength) & 0x80) == 0)
		{
			break;			/*	Out of loop.	*/
		}

		sdnvLength++;
		if (sdnvLength < 10)
		{
			continue;
		}

		putErrmsg("Duct number SDNV is too long.", NULL);
		*parms->authenticated = 1;	/*	Inauthentic.	*/
		terminateReceiverThread(parms);
		MRELEASE(parms);
		return NULL;
	}

	oK(decodeSdnv(&ductNbr, sdnvText));
	if (ductNbr < parms->baseDuctNbr || ductNbr > parms->lastDuctNbr)
	{
		putErrmsg("Duct number is too large.", utoa(ductNbr));
		*parms->authenticated = 1;	/*	Inauthentic.	*/
		terminateReceiverThread(parms);
		MRELEASE(parms);
		return NULL;
	}

	parms->ductNbr = ductNbr;
	senderSocket = ductNbr - parms->baseDuctNbr;
	if (parms->brsSockets[senderSocket] != -1)
	{
		putErrmsg("Client is already connected.", utoa(ductNbr));
		*parms->authenticated = 1;	/*	Inauthentic.	*/
		terminateReceiverThread(parms);
		MRELEASE(parms);
		return NULL;
	}

	isprintf(keyName, sizeof keyName, "%u.brs", ductNbr);
	keyLen = sec_get_key(keyName, &keyBufLen, key);
	if (keyLen == 0)
	{
		putErrmsg("Can't get HMAC key for duct.", keyName);
		*parms->authenticated = 1;	/*	Inauthentic.	*/
		terminateReceiverThread(parms);
		MRELEASE(parms);
		return NULL;
	}

	/*	Get time tag and its HMAC-SHA1 digest.			*/

	switch (receiveBytesByTCP(parms->bundleSocket, registration,
			REGISTRATION_LEN))
	{
		case REGISTRATION_LEN:
			break;			/*	Out of switch.	*/

		case -1:
			putErrmsg("Can't get registration.", NULL);

			/*	Intentional fall-through to next case.	*/

		default:
			*parms->authenticated = 1;
			terminateReceiverThread(parms);
			MRELEASE(parms);
			return NULL;
	}

	memcpy((char *) &timeTag, registration, 4);
	timeTag = ntohl(timeTag);
	if (timeTag - currentTime > BRSTERM || currentTime - timeTag > BRSTERM)
	{
		isprintf(errtext, sizeof errtext, "[?] Registration rejected: \
time tag is %u, must be between %u and %u.", (unsigned int) timeTag,
				(unsigned int) (currentTime - BRSTERM),
				(unsigned int) (currentTime + BRSTERM));
		writeMemo(errtext);
		*parms->authenticated = 1;	/*	Inauthentic.	*/
		terminateReceiverThread(parms);
		MRELEASE(parms);
		return NULL;
	}

	oK(hmac_authenticate(digest, DIGEST_LEN, key, keyLen, registration, 4));
	if (memcmp(digest, registration + 4, DIGEST_LEN) != 0)
	{
		writeMemo("[?] Registration rejected: digest is incorrect.");
		*parms->authenticated = 1;	/*	Inauthentic.	*/
		terminateReceiverThread(parms);
		MRELEASE(parms);
		return NULL;
	}

	*parms->authenticated = 2;		/*	Authentic.	*/
	parms->brsSockets[senderSocket] = parms->bundleSocket;

	/*	Client is authenticated.  Now authenticate self to
	 *	the client and continue.				*/

	timeTag++;
	timeTag = htonl(timeTag);
	oK(hmac_authenticate(digest, DIGEST_LEN, key, keyLen,
			(char *) &timeTag, 4));
	memcpy(registration + 4, digest, DIGEST_LEN);
	if (sendBytesByTCP(&parms->bundleSocket, registration + 4,
			DIGEST_LEN) < DIGEST_LEN)
	{
		putErrmsg("Can't countersign client.",
				itoa(parms->bundleSocket));
		terminateReceiverThread(parms);
		MRELEASE(parms);
		return NULL;
	}

	/*	Reforward bundles that are awaiting custody signals
 	 *	that will never arrive (because the bundles were
 	 *	sent into the bit bucket when dequeued, due to brsc
 	 *	disconnection).						*/

	if (reforwardStrandedBundles(ductNbr) < 0)
	{
		putErrmsg("brssscla can't reforward bundles.", NULL);
		terminateReceiverThread(parms);
		MRELEASE(parms);
		return NULL;
	}

	work = bpGetAcqArea(parms->vduct);
	if (work == NULL)
	{
		putErrmsg("brsscla can't get acquisition work area.", NULL);
		terminateReceiverThread(parms);
		MRELEASE(parms);
		return NULL;
	}

	buffer = MTAKE(TCPCLA_BUFSZ);
	if (buffer == NULL)
	{
		putErrmsg("brsscla can't get TCP buffer.", NULL);
		terminateReceiverThread(parms);
		MRELEASE(parms);
		return NULL;
	}

	parms->senderEid = parms->senderEidBuffer;
	isprintf(parms->senderEidBuffer, sizeof parms->senderEidBuffer,
			"ipn:%lu.0", ductNbr);

	/*	Now start receiving bundles.				*/

	while (threadRunning)
	{
		if (bpBeginAcq(work, 0, parms->senderEid) < 0)
		{
			putErrmsg("can't begin acquisition of bundle.", NULL);
			threadRunning = 0;
			continue;
		}

		switch (receiveBundleByTcp(parms->bundleSocket, work, buffer))
		{
		case -1:
			putErrmsg("Can't acquire bundle.", NULL);

			/*	Intentional fall-through to next case.	*/

		case 0:				/*	Normal stop.	*/
			threadRunning = 0;
			continue;

		default:
			break;			/*	Out of switch.	*/
		}

		if (bpEndAcq(work) < 0)
		{
			putErrmsg("Can't end acquisition of bundle.", NULL);
			threadRunning = 0;
			continue;
		}

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}

	/*	Finish releasing receiver thread's resources.		*/

	bpReleaseAcqArea(work);
	MRELEASE(buffer);
	terminateReceiverThread(parms);
	MRELEASE(parms);
	return NULL;
}

/*	*	*	Access thread functions	*	*	*	*/

typedef struct
{
	VInduct			*vduct;
	struct sockaddr		socketName;
	struct sockaddr_in	*inetName;
	int			ductSocket;
	int			running;
	int			baseDuctNbr;
	int			lastDuctNbr;
	int			*brsSockets;
} AccessThreadParms;

static void	*spawnReceivers(void *parm)
{
	/*	Main loop for acceptance of connections and
	 *	creation of receivers to service those connections.	*/

	AccessThreadParms	*atp = (AccessThreadParms *) parm;
	pthread_mutex_t		mutex;
	Lyst			threads;
	int			newSocket;
	struct sockaddr		clientSocketName;
	socklen_t		nameLength = sizeof(struct sockaddr);
	ReceiverThreadParms	*receiverParms;
	int			authenticated;
	LystElt			elt;
	pthread_t		thread;

	pthread_mutex_init(&mutex, NULL);
	threads = lyst_create_using(getIonMemoryMgr());
	if (threads == NULL)
	{
		putErrmsg("brsscla can't create threads list.", NULL);
		pthread_mutex_destroy(&mutex);
		return NULL;
	}

	/*	Can now begin accepting connections from clients.  On
	 *	failure, take down the whole CLI.			*/

	while (1)
	{
		newSocket = accept(atp->ductSocket, &clientSocketName,
				&nameLength);
		if (newSocket < 0)
		{
			putSysErrmsg("brsscla accept() failed", NULL);
			pthread_mutex_destroy(&mutex);
			pthread_kill(brssclaMainThread(0), SIGTERM);
			continue;
		}

		if (atp->running == 0)
		{
			break;	/*	Main thread has shut down.	*/
		}

		receiverParms = (ReceiverThreadParms *)
				MTAKE(sizeof(ReceiverThreadParms));
		if (receiverParms == NULL)
		{
			putErrmsg("brsscla can't allocate for thread", NULL);
			pthread_mutex_destroy(&mutex);
			pthread_kill(brssclaMainThread(0), SIGTERM);
			continue;
		}

		pthread_mutex_lock(&mutex);
		receiverParms->vduct = atp->vduct;
		receiverParms->elt = lyst_insert_last(threads, receiverParms);
		pthread_mutex_unlock(&mutex);
		if (receiverParms->elt == NULL)
		{
			putErrmsg("brsscla can't allocate for thread", NULL);
			MRELEASE(receiverParms);
			pthread_mutex_destroy(&mutex);
			pthread_kill(brssclaMainThread(0), SIGTERM);
			continue;
		}

		receiverParms->mutex = &mutex;
		receiverParms->bundleSocket = newSocket;
		authenticated = 0;		/*	Unknown.	*/
		receiverParms->authenticated = &authenticated;
		receiverParms->ductNbr = (unsigned long) -1;
		receiverParms->baseDuctNbr = atp->baseDuctNbr;
		receiverParms->lastDuctNbr = atp->lastDuctNbr;
		receiverParms->brsSockets = atp->brsSockets;
		if (pthread_create(&(receiverParms->thread), NULL,
				receiveBundles, receiverParms))
		{
			putSysErrmsg("brsscla can't create new thread", NULL);
			MRELEASE(receiverParms);
			pthread_mutex_destroy(&mutex);
			pthread_kill(brssclaMainThread(0), SIGTERM);
			continue;
		}

		snooze(1);		/*	Wait for authenticator.	*/
		if (authenticated == 0)	/*	Still unknown.		*/
		{
			/*	Assume hung on DOS attack.  Bail out.	*/

			thread = receiverParms->thread;
			pthread_cancel(thread);
			terminateReceiverThread(receiverParms);
			MRELEASE(receiverParms);
			pthread_join(thread, NULL);
		}
	}

	close(atp->ductSocket);
	writeErrmsgMemos();

	/*	Shut down all clients' receiver threads cleanly.	*/

	while (1)
	{
		pthread_mutex_lock(&mutex);
		elt = lyst_first(threads);
		if (elt == NULL)	/*	All threads shut down.	*/
		{
			pthread_mutex_unlock(&mutex);
			break;
		}

		/*	Trigger termination of thread.			*/

		receiverParms = (ReceiverThreadParms *) lyst_data(elt);
		pthread_mutex_unlock(&mutex);
		thread = receiverParms->thread;
		pthread_kill(thread, SIGTERM);
		pthread_join(thread, NULL);
		close(receiverParms->bundleSocket);
	}

	lyst_destroy(threads);
	writeErrmsgMemos();
	writeMemo("[i] brsscla access thread has ended.");
	pthread_mutex_destroy(&mutex);
	return NULL;
}

/*	*	*	Main thread functions	*	*	*	*/

static int	run_brsscla(char *ductName, int baseDuctNbr, int lastDuctNbr,
			int *brsSockets)
{
	VInduct			*vinduct;
	PsmAddress		vductElt;
	VOutduct		*voutduct;
	Sdr			sdr;
	Induct			induct;
	ClProtocol		protocol;
	char			*hostName;
	unsigned short		portNbr;
	unsigned int		hostNbr;
	AccessThreadParms	atp;
	socklen_t		nameLength;
	pthread_t		accessThread;
	SenderThreadParms	senderParms;
	pthread_t		senderThread;
	int			fd;

	findInduct("brss", ductName, &vinduct, &vductElt);
	if (vductElt == 0)
	{
		putErrmsg("No such brss induct.", ductName);
		return 1;
	}

	if (vinduct->cliPid > 0 && vinduct->cliPid != sm_TaskIdSelf())
	{
		putErrmsg("CLI task is already started for this duct.",
				itoa(vinduct->cliPid));
		return 1;
	}

	findOutduct("brss", ductName, &voutduct, &vductElt);
	if (vductElt == 0)
	{
		putErrmsg("No such brss outduct.", ductName);
		return 1;
	}

	/*	All command-line arguments are now validated.		*/

	sdr = getIonsdr();
	sdr_read(sdr, (char *) &induct, sdr_list_data(sdr, vinduct->inductElt),
			sizeof(Induct));
	sdr_read(sdr, (char *) &protocol, induct.protocol, sizeof(ClProtocol));
	if (protocol.nominalRate <= 0)
	{
		vinduct->acqThrottle.nominalRate = DEFAULT_BRS_RATE;
		voutduct->xmitThrottle.nominalRate = DEFAULT_BRS_RATE;
	}
	else
	{
		vinduct->acqThrottle.nominalRate = protocol.nominalRate;
		voutduct->xmitThrottle.nominalRate = protocol.nominalRate;
	}

	hostName = ductName;
	parseSocketSpec(ductName, &portNbr, &hostNbr);
	if (portNbr == 0)
	{
		portNbr = 80;
	}

	portNbr = htons(portNbr);
	if (hostNbr == 0)
	{
		putErrmsg("Can't get IP address for host.", hostName);
		return 1;
	}

	atp.vduct = vinduct;
	hostNbr = htonl(hostNbr);
	memset((char *) &(atp.socketName), 0, sizeof(struct sockaddr));
	atp.inetName = (struct sockaddr_in *) &(atp.socketName);
	atp.inetName->sin_family = AF_INET;
	atp.inetName->sin_port = portNbr;
	memcpy((char *) &(atp.inetName->sin_addr.s_addr), (char *) &hostNbr, 4);
	atp.ductSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (atp.ductSocket < 0)
	{
		putSysErrmsg("Can't open TCP socket", NULL);
		return 1;
	}

	nameLength = sizeof(struct sockaddr);
	if (reUseAddress(atp.ductSocket)
	|| bind(atp.ductSocket, &(atp.socketName), nameLength) < 0
	|| listen(atp.ductSocket, 5) < 0
	|| getsockname(atp.ductSocket, &(atp.socketName), &nameLength) < 0)
	{
		close(atp.ductSocket);
		putSysErrmsg("Can't initialize socket (note: must be root for \
port 80)", NULL);
		return 1;
	}

	/*	Initialize sender endpoint ID lookup.			*/

	ipnInit();
	dtn2Init();

	/*	Set up signal handling.  SIGTERM is shutdown signal.	*/

	oK(brssclaMainThread(pthread_self()));
	isignal(SIGTERM, interruptThread);
	isignal(SIGPIPE, handleConnectionLoss);

	/*	Start the sender thread; a single sender for all
	 *	connections.						*/

	senderParms.vduct = voutduct;
	senderParms.baseDuctNbr = baseDuctNbr;
	senderParms.lastDuctNbr = lastDuctNbr;
	senderParms.brsSockets = brsSockets;
	if (pthread_create(&senderThread, NULL, sendBundles, &senderParms))
	{
		close(atp.ductSocket);
		putSysErrmsg("brsscla can't create sender thread", NULL);
		return 1;
	}

	/*	Start the access thread.				*/

	atp.running = 1;
	atp.baseDuctNbr = baseDuctNbr;
	atp.lastDuctNbr = lastDuctNbr;
	atp.brsSockets = brsSockets;
	if (pthread_create(&accessThread, NULL, spawnReceivers, &atp))
	{
		sm_SemEnd(voutduct->semaphore);
		pthread_join(senderThread, NULL);
		close(atp.ductSocket);
		putSysErrmsg("brsscla can't create access thread", NULL);
		return 1;
	}

	/*	Now sleep until interrupted by SIGTERM, at which point
	 *	it's time to stop the induct.				*/

	writeMemo("[i] brsscla is running.");
	snooze(2000000000);

	/*	Time to shut down.					*/

	atp.running = 0;

	/*	Shut down sender thread cleanly.			*/

	if (voutduct->semaphore != SM_SEM_NONE)
	{
		sm_SemEnd(voutduct->semaphore);
	}

	pthread_join(senderThread, NULL);

	/*	Wake up the access thread by connecting to it.		*/

	fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd >= 0)
	{
		connect(fd, &(atp.socketName), sizeof(struct sockaddr));

		/*	Immediately discard the connected socket.	*/

		close(fd);
	}

	pthread_join(accessThread, NULL);
	writeErrmsgMemos();
	writeMemo("[i] brsscla induct has ended.");
	return 0;
}

/*	Each member of the brsSockets table is the FD of the open
 *	socket (if any) that has been established for communication
 *	with the BRS client identified by the duct number whose value
 *	is the base duct number for this BRS daemon plus this member's
 *	position within the table.  That is, if the base duct number
 *	for the server is 1 then brsSockets[19] is the FD for
 *	communicating with the BRS client identified by BRS duct
 *	number 20.							*/

#if defined (VXWORKS) || defined (RTEMS)
int	brsscla(int a1, int a2, int a3, int a4, int a5,
		int a6, int a7, int a8, int a9, int a10)
{
	char	*ductName = (char *) a1;
	int	baseDuctNbr = a2 ? atoi((char *) a2) : 1;
	int	lastDuctNbr = a3 ? atoi((char *) a3) : baseDuctNbr + 255;
	int	scope;
	int	i;
	int	result;
	int	*brsSockets;
#else
int	main(int argc, char *argv[])
{
	char	*ductName = argc > 1 ? argv[1] : NULL;
	int	baseDuctNbr = argc > 2 ? atoi(argv[2]) : 1;
	int	lastDuctNbr = argc > 3 ? atoi(argv[3]) : baseDuctNbr + 255;
	int	scope;
	int	i;
	int	result;
	int	*brsSockets;

	if (ductName == NULL)
	{
		PUTS("Usage: brsscla <local host name>[:<port number>] \
[<first duct nbr in scope>[ <last duct nbr in scope>]]");
		return 0;
	}
#endif
	if (bpAttach() < 0)
	{
		return 1;
	}

	if (baseDuctNbr < 1 || lastDuctNbr < baseDuctNbr)
	{
		putErrmsg("brsscla scope error: first duct nbr must be > 0, \
last ductNbr must not be less than first.", itoa(baseDuctNbr));
		return 1;
	}

	scope = lastDuctNbr - baseDuctNbr;
	brsSockets = (int *) MTAKE(sizeof(int) * scope);
	if (brsSockets == NULL)
	{
		putErrmsg("Can't allocate BRS sockets array.", itoa(scope));
		return 1;
	}

	for (i = 0; i < scope; i++)
	{
		brsSockets[i] = -1;
	}

	result = run_brsscla(ductName, baseDuctNbr, lastDuctNbr, brsSockets);
	MRELEASE(brsSockets);
	bp_detach();
	return result;
}
