/*
	udplsi.c:	LTP UDP-based link service daemon.

	Author: Scott Burleigh, JPL

	Copyright (c) 2007, California Institute of Technology.
	ALL RIGHTS RESERVED.  U.S. Government Sponsorship
	acknowledged.
	
									*/
#include "udplsa.h"

static void	interruptThread()
{
	isignal(SIGTERM, interruptThread);
}

/*	*	*	Receiver thread functions	*	*	*/

typedef struct
{
	int		linkSocket;
	pthread_t	mainThread;
	int		running;
} ReceiverThreadParms;

static void	*handleDatagrams(void *parm)
{
	/*	Main loop for UDP datagram reception and handling.	*/

	ReceiverThreadParms	*rtp = (ReceiverThreadParms *) parm;
	char			*buffer;
	int			segmentLength;
	struct sockaddr_in	fromAddr;
	socklen_t		fromSize;

	buffer = MTAKE(UDPLSA_BUFSZ);
	if (buffer == NULL)
	{
		putErrmsg("udplsi can't get UDP buffer.", NULL);
		pthread_kill(rtp->mainThread, SIGTERM);
		return NULL;
	}

	/*	Can now start receiving bundles.  On failure, take
	 *	down the LSI.						*/

	iblock(SIGTERM);
	while (rtp->running)
	{	
		fromSize = sizeof fromAddr;
		segmentLength = recvfrom(rtp->linkSocket, buffer, UDPLSA_BUFSZ,
				0, (struct sockaddr *) &fromAddr, &fromSize);
		switch(segmentLength)
		{
		case -1:
			putSysErrmsg("Can't acquire segment", NULL);
			pthread_kill(rtp->mainThread, SIGTERM);

			/*	Intentional fall-through to next case.	*/

		case 1:				/*	Normal stop.	*/
			rtp->running = 0;
			continue;
		}

		if (ltpHandleInboundSegment(buffer, segmentLength) < 0)
		{
			putErrmsg("Can't handle inbound segment.", NULL);
			pthread_kill(rtp->mainThread, SIGTERM);
			rtp->running = 0;
			continue;
		}

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}

	writeErrmsgMemos();
	writeMemo("[i] udplsi receiver thread has ended.");

	/*	Free resources.						*/

	MRELEASE(buffer);
	return NULL;
}

/*	*	*	Main thread functions	*	*	*	*/

#if defined (VXWORKS) || defined (RTEMS)
int	udplsi(int a1, int a2, int a3, int a4, int a5,
		int a6, int a7, int a8, int a9, int a10)
{
	char	*endpointSpec = (char *) a1;
#else
int	main(int argc, char *argv[])
{
	char	*endpointSpec = (argc > 1 ? argv[1] : NULL);
#endif
	LtpVdb			*vdb;
	unsigned short		portNbr = 0;
	unsigned int		ipAddress = 0;
	char			ownHostName[MAXHOSTNAMELEN];
	struct sockaddr		socketName;
	struct sockaddr_in	*inetName;
	ReceiverThreadParms	rtp;
	socklen_t		nameLength;
	pthread_t		receiverThread;
	int			fd;
	char			quit = '\0';

	/*	Note that ltpadmin must be run before the first
	 *	invocation of ltplsi, to initialize the LTP database
	 *	(as necessary) and dynamic database.			*/ 

	if (ltpInit(0, 0) < 0)
	{
		putErrmsg("udplsi can't initialize LTP.", NULL);
		return 1;
	}

	vdb = getLtpVdb();
	if (vdb->lsiPid > 0 && vdb->lsiPid != sm_TaskIdSelf())
	{
		putErrmsg("LSI task is already started.", itoa(vdb->lsiPid));
		return 1;
	}

	/*	All command-line arguments are now validated.		*/

	if (endpointSpec)
	{
		parseSocketSpec(endpointSpec, &portNbr, &ipAddress);
	}

	if (portNbr == 0)
	{
		portNbr = LtpUdpDefaultPortNbr;
	}
	
	if (ipAddress == 0)		/*	Default to local host.	*/
	{
		getNameOfHost(ownHostName, sizeof ownHostName);
		ipAddress = getInternetAddress(ownHostName);
	}

	portNbr = htons(portNbr);
	ipAddress = htonl(ipAddress);
	memset((char *) &socketName, 0, sizeof socketName);
	inetName = (struct sockaddr_in *) &socketName;
	inetName->sin_family = AF_INET;
	inetName->sin_port = portNbr;
	memcpy((char *) &(inetName->sin_addr.s_addr), (char *) &ipAddress, 4);
	rtp.linkSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (rtp.linkSocket < 0)
	{
		putSysErrmsg("LSI can't open UDP socket", NULL);
		return -1;
	}

	nameLength = sizeof(struct sockaddr);
	if (reUseAddress(rtp.linkSocket)
	|| bind(rtp.linkSocket, &socketName, nameLength) < 0
	|| getsockname(rtp.linkSocket, &socketName, &nameLength) < 0)
	{
		close(rtp.linkSocket);
		putSysErrmsg("Can't initialize socket", NULL);
		return 1;
	}

	/*	Set up signal handling; SIGTERM is shutdown signal.	*/

	isignal(SIGTERM, interruptThread);

	/*	Start the receiver thread.				*/

	rtp.running = 1;
	rtp.mainThread = pthread_self();
	if (pthread_create(&receiverThread, NULL, handleDatagrams, &rtp))
	{
		close(rtp.linkSocket);
		putSysErrmsg("udplsi can't create receiver thread", NULL);
		return 1;
	}

	/*	Now sleep until interrupted by SIGTERM, at which point
	 *	it's time to stop the link service.			*/

	writeMemo("[i] udplsi is running.");
	snooze(2000000000);

	/*	Time to shut down.					*/

	rtp.running = 0;

	/*	Wake up the receiver thread by sending it a 1-byte
	 *	datagram.						*/

	fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd >= 0)
	{
		sendto(fd, &quit, 1, 0, &socketName, sizeof(struct sockaddr));
		close(fd);
	}

	pthread_join(receiverThread, NULL);
	close(rtp.linkSocket);
	writeErrmsgMemos();
	writeMemo("[i] udplsi duct has ended.");
	return 0;
}
