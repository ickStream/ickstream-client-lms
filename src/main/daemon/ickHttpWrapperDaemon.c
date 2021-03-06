#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "ickP2p.h"

char* wrapperURL = NULL;
char wrapperIP[16];
int wrapperPort = 80;
char wrapperPath[1024];
char* wrapperAuthorization = NULL;
int bShutdown = 0;
ickP2pContext_t* g_context = NULL;

char* httpRequest(const char* ip, int port, const char* path, const char* authorization, const char* requestData)
{
	int SIZE = 1023;
	char buffer[SIZE+1];
	char *request = NULL;
    char *responseData = NULL;
	char* responseBody = NULL;
    struct sockaddr_in * server_addr = NULL;
	int server_socket = socket(AF_INET, SOCK_STREAM, 0);
	if(server_socket < 0) {
		fprintf(stderr, "Unable to open socket: %d\n",server_socket);
        fflush (stderr);
		goto httpRequest_end;
	}

	//printf("Getting address for %s and port %d\n",ip,port);
	server_addr = (struct sockaddr_in *) malloc(sizeof(struct sockaddr_in));
	server_addr->sin_family = AF_INET;
	server_addr->sin_port = htons(port);
	if(inet_pton(AF_INET,ip,(void *)(&(server_addr->sin_addr.s_addr))) <= 0) {
		fprintf(stderr,"Unable to convert string IP to byte IP using inet_pton\n");
        fflush (stderr);
		goto httpRequest_end;
	}
	bzero(&(server_addr->sin_zero), 8);
	
	struct timeval tv;
	tv.tv_sec = 30;  // 30 Secs Timeout
	tv.tv_usec = 0;  // Not init'ing this can cause strange errors
	setsockopt(server_socket,SOL_SOCKET,SO_RCVTIMEO,(char *)&tv,sizeof(struct timeval));

	int rc = connect(server_socket, 
		(struct sockaddr *) server_addr, 
		sizeof(struct sockaddr));
	if(rc < 0) {
		fprintf(stderr, "Fail to connect to socket: %d\n",errno);
        fflush (stderr);
		goto httpRequest_end;
	}
	char *requestHeader;
	const int CONTENT_LENGTH_SPACE = 10; // just allocate enough
	if(authorization != NULL) {
		requestHeader = "POST /%s HTTP/1.0\r\nHost: %s\r\nAuthorization: Basic %s\r\nX-Scanner: 1\r\nUser-Agent: ickHttpWrapperDaemon/1.0\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s";
		request = (char*)malloc(strlen(requestHeader)+strlen(authorization)+strlen(ip)+strlen(path)+CONTENT_LENGTH_SPACE+strlen(requestData)-8+1+23);
		sprintf(request,requestHeader,path,ip,authorization,strlen(requestData),requestData);
	}else {
		requestHeader = "POST /%s HTTP/1.0\r\nHost: %s\r\nUser-Agent: ickHttpWrapperDaemon/1.0\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s";
		request = (char*)malloc(strlen(requestHeader)+strlen(ip)+strlen(path)+CONTENT_LENGTH_SPACE+strlen(requestData)-8+1);
		sprintf(request,requestHeader,path,ip,strlen(requestData),requestData);
	}
	int sent = 0;
	while(sent < strlen(request)) {
		//printf("Forwarding request: ===============\n%s\n==============\n",request);
		int bytes_sent = send(server_socket, request, strlen(request), 0);
		if(bytes_sent < 0) {
			fprintf(stderr, "Error when forwarding request data: %d\n",bytes_sent);
	        fflush (stderr);
			goto httpRequest_end;
		}
		sent = sent + bytes_sent;
	}
	printf("Request successfully sent to perl module via HTTP\n");
	fflush (stdout);

	//printf("Starting to read data\n");
	int bytes_received = 0;
	int total_bytes_received = 0;
	while((bytes_received = recv(server_socket, buffer, SIZE, 0)) > 0) {
		buffer[bytes_received] = '\0';
		responseData = realloc(responseData,total_bytes_received+bytes_received+1);
		if(responseData != NULL) {
			memcpy(responseData+total_bytes_received, buffer, bytes_received + 1);
			total_bytes_received += bytes_received;
		}else {
			fprintf(stderr, "Error allocating memory for response via HTTP\n");
	        fflush (stderr);
			goto httpRequest_end;
		}
	}
	if(bytes_received<0) {
			fprintf(stderr, "Error reading response via HTTP: %d\n",errno);
	        fflush (stderr);
	}

	//printf("Finished reading, total=%d, last=%d\n",total_bytes_received,bytes_received);
	if(responseData != NULL) {
		char *bodyOffset = strstr(responseData, "\r\n\r\n");
		if(bodyOffset != NULL) {
			int bodySize = total_bytes_received-(bodyOffset+4-responseData)+1;
			responseBody = malloc(bodySize);
			memcpy(responseBody, bodyOffset+4, bodySize);
			//printf("Got body:\n%s\n",responseBody);
		}
	}
httpRequest_end:
	if(request) {
		free(request);
	}
	if(server_socket>=0) {
		close(server_socket);
	}
	if(server_addr) {
		free(server_addr);
	}
	if(responseData != NULL) {
		free(responseData);
	}
	return responseBody;
}

void discoveryCb(ickP2pContext_t *ictx, const char *szDeviceId, ickP2pDeviceState_t change, ickP2pServicetype_t type)
{
    printf("DISCOVERY %s type=%d services=%d)\n",szDeviceId,(int)change,(int)type);
	fflush (stdout);
}

void messageCb(ickP2pContext_t *ictx, const char *szSourceDeviceId, ickP2pServicetype_t sourceService, ickP2pServicetype_t targetService, const char* message, size_t messageLength, ickP2pMessageFlag_t mFlags )
{
	char* terminatedMessage = NULL;
	if(messageLength>0) {
		terminatedMessage = malloc(messageLength+1);
		memcpy(terminatedMessage,message,messageLength);
		terminatedMessage[(int)messageLength]='\0';
		printf("From %s: %s\n",szSourceDeviceId, terminatedMessage);
	}else {
		printf("From %s: %s\n",szSourceDeviceId, message);
	}
	fflush (stdout);
	char* response = NULL;
	if(terminatedMessage != NULL) {
		response = httpRequest(wrapperIP, wrapperPort, wrapperPath,wrapperAuthorization, terminatedMessage);
	}else {
		response = httpRequest(wrapperIP, wrapperPort, wrapperPath,wrapperAuthorization, message);
	}
    if( response ) {
        printf("To %s: %s\n",szSourceDeviceId, response);
        fflush (stdout);
        ickErrcode_t error = ickP2pSendMsg(ictx,szSourceDeviceId, sourceService,ICKP2P_SERVICE_SERVER_GENERIC,response, strlen(response));
        if(error != ICKERR_SUCCESS) {
    		fprintf(stderr,"Failed to send response=%d\n",(int)error);
	        fflush (stderr);
    	}
    }
	if(response != NULL) {
		free(response);
		response = NULL;
	}
	if(terminatedMessage != NULL) {
		free(terminatedMessage);
		terminatedMessage = NULL;
	}
}
	
static void shutdownHandler( int sig, siginfo_t *siginfo, void *context )
{
    switch( sig) {
        case SIGINT:
        case SIGTERM:
            bShutdown = sig;
            break;
        default:
            break;
	}
}

int main( int argc, char *argv[] )
{
	if(argc != 6 && argc != 7) {
		printf("Usage: %s IP-address deviceId deviceName wrapperURL logFile\n",argv[0]);
		return 0;
	}
    char* networkAddress = argv[1];
	char* deviceId = argv[2];
	char* deviceName = argv[3];
	wrapperURL = argv[4];
	char* logFile = argv[5];
	if(argc == 7) {
		wrapperAuthorization = argv[6];
	}
	
    char host[100];
	memset(wrapperPath, 0, 1024);
	memset(host, 0, 100);
	printf("Parsing url...%s\n",wrapperURL);
	sscanf(wrapperURL, "http://%99[^:]:%99d/%99[^\n]", host, &wrapperPort, wrapperPath);
	printf("Parsed...\n");
	if(strlen(host)==0) {
		printf("Unable to parse host from URL\n");
		return 0;
	}else {
		printf("Parsed url: %s, %d, %s\n",host,wrapperPort,wrapperPath);
		struct hostent *hent = NULL;
		memset(wrapperIP, 0, 16);
		if((hent = (struct hostent *)gethostbyname(host)) == NULL)
		{
			printf("Unable to get host information for hostname\n");
			return 0;
		}
		if(inet_ntop(AF_INET, (void *)hent->h_addr_list[0], wrapperIP, 15) == NULL)
		{
			printf("Can't resolve host to IP\n");
			return 0;
		}
	}
	if(strlen(wrapperPath)==0) {
		printf("Unable to parse path from URL\n");
		return 0;
	}

    int fd1 = open( "/dev/null", O_RDWR, 0 );
    int fd2 = open( logFile, O_RDWR|O_CREAT|O_TRUNC, 0644 );
    if( fd1!=-1) {
      dup2(fd1, fileno(stdin));
    }
    if( fd2!=-1) {
      dup2(fd2, fileno(stdout));
      dup2(fd2, fileno(stderr));
    }

#ifdef DEBUG
    printf("ickP2pSetLogLevel(7)\n");
    ickP2pSetLogging(7,stderr,100);
#elif ICK_DEBUG
    ickP2pSetLogging(6,NULL,100);
#endif

    printf("Initializing ickP2P for %s(%s) at %s...\n",deviceName,deviceId,networkAddress);
    printf("Wrapping URL: %s\n",wrapperURL);
    printf("- Using IP-address: %s\n",wrapperIP);
    printf("- Using port: %d\n",wrapperPort);
    printf("- Using path: /%s\n",wrapperPath);
    
	ickErrcode_t error;
    printf("create(\"%s\",\"%s\",NULL,0,0,%d,%p)\n",deviceName,deviceId,ICKP2P_SERVICE_SERVER_GENERIC,&error);
	g_context = ickP2pCreate(deviceName,deviceId,NULL,0,0,ICKP2P_SERVICE_SERVER_GENERIC,&error);
	if(error == ICKERR_SUCCESS) {
    	error = ickP2pRegisterMessageCallback(g_context, &messageCb);
    	if(error != ICKERR_SUCCESS) {
    		fprintf(stderr, "ickP2pRegisterMessageCallback failed=%d\n",(int)error);
	        fflush (stderr);
    	}
    	error = ickP2pRegisterDiscoveryCallback(g_context, &discoveryCb);
    	if(error != ICKERR_SUCCESS) {
    		fprintf(stderr, "ickP2pRegisterDiscoveryCallback failed=%d\n",(int)error);
	        fflush (stderr);
    	}
#ifdef ICK_DEBUG
	    ickP2pSetHttpDebugging(g_context,1);
#endif
		error = ickP2pAddInterface(g_context, networkAddress, NULL);
    	if(error != ICKERR_SUCCESS) {
    		fprintf(stderr, "ickP2pAddInterface failed=%d\n",(int)error);
	        fflush (stderr);
    	}
    	error = ickP2pResume(g_context);
    	if(error != ICKERR_SUCCESS) {
    		fprintf(stderr, "ickP2pResume failed=%d\n",(int)error);
	        fflush (stderr);
    	}
		
	}else {
   		fprintf(stderr, "ickP2pCreate failed=%d\n",(int)error);
        fflush (stderr);
	}
	fflush (stdout);

    struct sigaction act;
    memset( &act, 0, sizeof(act) );
    act.sa_sigaction = &shutdownHandler;
    act.sa_flags     = SA_SIGINFO;
    sigaction( SIGINT, &act, NULL );
    sigaction( SIGTERM, &act, NULL );

    while (!bShutdown) {
    	sleep(1000);
    }
    printf("Shutting down ickP2P for %s\n",deviceName);
    ickP2pEnd(g_context,NULL);
    printf("Shutdown ickP2P for %s\n",deviceName);
	return 1;
}
