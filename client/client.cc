

#define NOLOGATALL
#include "../source/acbuf.cc"
#include "../include/sockio.h"
#include "../include/fileio.h"

#include <unistd.h>
#include <sys/select.h>
#include <signal.h>

int main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);
	
	acbuf bufToD, bufFromD;

	int in=fileno(stdin);
	int out=fileno(stdout);

	if (in<0 || out<0)
		return -1; // hm?!


	if (argc<2)
	{
		printf("ERROR: Missing socket path argument, check inetd.conf.\n");
		return 1;
	}
	size_t pLen=strlen(argv[1]);
	if (pLen>107)
	{
		printf("ERROR: Bad socket path, too long.\n");
		return -2;
	}

	struct stat stinfo;
	if (0!=stat(argv[1], &stinfo) || !S_ISSOCK(stinfo.st_mode))
	{
		printf("HTTP/1.1 500 ERROR: Socket file missing or in bad state. Check the command lines.\r\n\r\n");
		return 2;
	}

	struct sockaddr_storage ss;
	socklen_t slen = sizeof(ss);
	char hbuf[NI_MAXHOST];
		
#if 1
		
	if (0!=getpeername(in, (struct sockaddr *)&ss, &slen))
	{
		printf("ERROR: stdin is not a socket.\n");
		return 1;
	}
		
	
	if (getnameinfo((struct sockaddr*) &ss, sizeof(ss), hbuf, sizeof(hbuf), 
			NULL, 0, NI_NUMERICHOST))
	{
		printf("ERROR: could not resolve hostname\n");
		return 1;
	}
#else
	strcpy(hbuf, "localhost");
#endif
	
	bufToD.setsize(4000);
	bufFromD.setsize(16000);

	int n=snprintf(bufToD.wptr(), bufToD.freecapa(),
			"GET / HTTP/1.1\r\nX-Original-Source: %s\r\n\r\n", hbuf);

	if (n<0 || n==(int)bufToD.freecapa()) // weird... too long?
		return 3;

	bufToD.got(n);
	
	int s=socket(PF_UNIX, SOCK_STREAM, 0);
	if(s<0)
		return 4;
	
	struct sockaddr_un addr;
	
	addr.sun_family=PF_UNIX;
	strcpy(addr.sun_path, argv[1]);
	socklen_t adlen = pLen+1+offsetof(struct sockaddr_un, sun_path);
	if ( 0!=connect(s, (struct sockaddr*)&addr, adlen))
	{
		printf("HTTP/1.1 500 ERROR: Unable to attach to the local daemon: %s\r\n\r\n", strerror(errno));
		return 5;
	}
	
	int maxfd=1+std::max(in, std::max(out, s));

	while (true)
	{
		fd_set rfds, wfds;
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		
		if(bufToD.size()>0)
			FD_SET(s, &wfds);
					
		if(bufFromD.size()>0)
			FD_SET(out, &wfds);
		
		if(bufFromD.freecapa()>0)
			FD_SET(s, &rfds);
		
		if(bufToD.freecapa()>0)
			FD_SET(in, &rfds);
		
		int nReady=select(maxfd, &rfds, &wfds, NULL, NULL);
		if (nReady<0)
		{
			fputs("Select failure.\n", stdout);
			exit(1);
		}
		
		if(FD_ISSET(s, &wfds))
		{
			if(bufToD.syswrite(s)<0)
				return 1;
		}
		
		if(FD_ISSET(out, &wfds))
		{
			if(bufFromD.syswrite(out)<0)
				return 1;
		}
		
		if(FD_ISSET(s, &rfds))
		{
			if(bufFromD.sysread(s)<=0)
				goto finished;
		}
		
		if(FD_ISSET(in, &rfds))
		{
			if(bufToD.sysread(in)<=0)
				goto finished;
		}
	}
    
	finished:
	forceclose(s);
	::shutdown(s, SHUT_RDWR);

	return 0;
}

