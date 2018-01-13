#include <sys/socket.h>
#include <sys/types.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <fcntl.h>

#include "buffer.h"

#define LISTEN_BACKLOG 128
#define BUFSIZE 8192
#define bool int
#define TRUE 1
#define FALSE 0
#define POOLSIZE 10

struct child_t{
	pthread_t tid;
	enum { T_EMPTY,T_WAIT,T_CONNECTED} status;
};
static struct child_t * child_ptr;
//thread pool uses this fd to accept incoming connections
//this fd is binded only once
static int serverfd;
static pthread_mutex_t childlock = PTHREAD_MUTEX_INITIALIZER;

int child_create(pthread_t * tid,void * pslot);

// proxy thread
void * proxy(void * pslot);
// proxy sub function
// it extracts host and port(if there is) from the request , connect to the host returns the socket
// or there is no host,returns -1
int connectHost(char * request,char * service);
//proxy sub function
//extract service (http,https likewise) from request
int getService(char * request,char * rlt);
int main(int argc,char ** argv)
{
	int serverport;
	if (argc != 2){
		fprintf(stderr,"usage:%s <port>\r\n",argv[0]);
		exit(-1);
	}
	serverport = atoi(argv[1]);
	if ((serverfd = socket(AF_INET,SOCK_STREAM,0)) < 0){
		fprintf(stderr,"socket faild,%s\r\n",strerror(errno));
		exit(-1);
	}
	//use setsockopt to enable ADDRESSREUSE,enable set optval = 1; disable set optval = 0
	int optval = 1;
	if ( setsockopt(serverfd,SOL_SOCKET,SO_REUSEADDR,(const void *)&optval,sizeof(int)) < 0){
		fprintf(stderr,"setsockopt error,%s\r\n",strerror(errno));
		exit(-1);
	}
	//bind address
	//struct sockaddr_in typedef uint32_t in_addr_t 
	//define INADDR_ANY ((in_addr_t) 0x00000000), ip addr A.B.C.D, A,B,C,D are between 0 - 255, which can be represented by 8 bits
	//typedef uint16_t in_port_t;
	printf("port %d\r\n",(in_port_t)serverport);
        struct sockaddr_in serveraddr;
	memset(&serveraddr,0,sizeof(struct sockaddr_in));
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_port = htons((in_port_t)serverport);
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind (serverfd,(struct sockaddr *)&serveraddr,sizeof(serveraddr)) < 0){
		fprintf(stderr,"bind error %s\r\n",strerror(errno));
		exit(-1);
	}
	printf("bind succeed\r\n");
	if (listen(serverfd,LISTEN_BACKLOG) < 0){
		fprintf(stderr,"bind error %s\r\n",strerror(errno));
		exit(-1);
	}	
	printf("listen succeed\r\n");
	//
	child_ptr = (struct child_t *)malloc(sizeof(struct child_t)*POOLSIZE);
	int i;
	for( i = 0; i < POOLSIZE; i++){
		pthread_t tid;
		if ( child_create(&tid,(void *)&i) == 0){
			child_ptr[i].tid = tid;
			child_ptr[i].status = T_WAIT;
		}
		else{
			child_ptr[i].status = T_EMPTY;
			continue;
		}

	}
	while(1){
		int j;
		for( j = 0; j < POOLSIZE; j++){	
			pthread_t tid;	
			pthread_mutex_lock(&childlock);
			if(child_ptr[j].status == T_EMPTY){
				if ( child_create(&tid,(void *)&i) == 0){
					child_ptr[j].tid = tid;
					child_ptr[j].status = T_WAIT;
				}
				else
					continue;

			}
			pthread_mutex_unlock(&childlock);
		}
	}
	return 0;		

}
int child_create(pthread_t * tid,void * pslot)
{
	printf("slot %d create\n",*((int *)pslot));
	return pthread_create(tid,NULL,proxy,pslot);
}
void * proxy(void * pslot)
{
	int slot = *((int *)pslot);
	struct sockaddr_in clientaddr;
	int clientlen = sizeof(clientaddr);
	int acceptfd;
	if( (acceptfd = accept(serverfd,(struct socketaddr *)&clientaddr,&clientlen)) < 0){
		fprintf(stderr,"accept error %s\r\n",strerror(errno));
		pthread_mutex_lock(&childlock);
		child_ptr[slot].status = T_EMPTY;
		pthread_mutex_unlock(&childlock);
		pthread_exit((void *) -1);
	}
	pthread_mutex_lock(&childlock);
	child_ptr[slot].status = T_CONNECTED;
	pthread_mutex_unlock(&childlock);
	printf("accept fd %d\r\n",acceptfd);

	
	char buf[BUFSIZE];
	bool b_service = FALSE;
        char service[10];
	char *pmethod;
	ssize_t len = 0;
	ssize_t cur = 0;
	
	int cnt = 0;
	int server;
	while ( (len = recv(acceptfd,&buf[cur],BUFSIZE - cur - 1,0)) > 0 ){
		printf("%d len=%d %s",cnt,len,&buf[cur]);
		if (!b_service)
			if(getService(buf,service))
				b_service = TRUE;
		cur += len;
		cnt ++;
		if( b_service){
			if (server = connectHost(buf,service))
				break;
			if (cur == BUFSIZE  )
				break;
		}
	}
	if ( (len < 0) || !(server > 0)){
		fprintf(stderr,"recv from %d error,%s\r\n",acceptfd,strerror(errno));
		pthread_mutex_lock(&childlock);
		child_ptr[slot].status = T_EMPTY;
		pthread_mutex_unlock(&childlock);
		pthread_exit((void *) -1);
	}
	fprintf(stderr,"connect to server fd %d\r\n",server);
	//send buf[0] - buf[cur-1] to server
	ssize_t size = 0;
	ssize_t tem = 0;
	while ( (cur  - size ) > 0){
		tem = send(server,&buf[size], cur - size, 0);
		size += tem;
	}
	fprintf(stderr,"send to server OK %d\r\n",size);
        //prepare select 
	fd_set rset,wset;

	int maxfd = (acceptfd) > server ? (acceptfd) : server;
	fprintf(stderr,"maxfd %d\r\n",maxfd);
	fcntl(acceptfd,F_SETFL,O_NONBLOCK);
	fcntl(server,F_SETFL,O_NONBLOCK);
	struct buffer_s * cbuffer;
	struct buffer_s * sbuffer;
	cbuffer = new_buffer();
	sbuffer = new_buffer();
	for(;;){
		FD_ZERO(&rset);
		FD_ZERO(&wset);
		
		FD_SET((acceptfd),&rset);
		FD_SET((acceptfd),&wset);
		
		FD_SET(server,&rset);
		FD_SET(server,&wset);
		

		select(maxfd+1,&rset,&wset,NULL,NULL);
		if(FD_ISSET(server,&rset)){
			if(read_buffer(server,sbuffer) < 0)
				break;
		}
		if(FD_ISSET(acceptfd,&rset)){
			if(read_buffer(acceptfd,cbuffer) < 0)
				break;
		}
		if(FD_ISSET(server,&wset)){
			if(write_buffer(server,cbuffer) < 0)
				break;
		}
		if(FD_ISSET(acceptfd,&wset)){
			if(write_buffer(acceptfd,sbuffer) < 0)
				break;	
		}				
	}
	int  flags = fcntl(acceptfd,F_GETFL,0);
	fcntl(acceptfd,F_SETFL,flags&~O_NONBLOCK);
	while( buffer_size(sbuffer) > 0){
		if (write_buffer(acceptfd, sbuffer) < 0)
			break;
	}
	shutdown(acceptfd, SHUT_WR);

	flags = fcntl(server,F_GETFL,0);
	fcntl(server,F_SETFL,flags&~O_NONBLOCK);
	while (buffer_size(cbuffer) > 0) {
		if (write_buffer(server, cbuffer) < 0)
			break;
	}
	pthread_mutex_lock(&childlock);
	child_ptr[slot].status = T_EMPTY;
	pthread_mutex_unlock(&childlock);
	pthread_exit((void *) 0); 
}
//extract service (http,https likewise) from request
int getService(char * request,char * rlt)
{
	char method[3][8] = {"CONNECT ","GET ","POST "};
	char * tem = NULL;
	char * start = NULL;
	char * end = NULL;
	if( (tem = strstr(request,method[0])) != NULL){
		start = &request[strlen(method[0])];
		if ( (end = strstr(start,":")) != NULL){
			start = end++;
			if ( ( end = strstr(start," ")) != NULL){
				strncpy(rlt,start,end - start);
				rlt[end - start] = '\0';
				return 1;
			}
		}
	}
	else if( (tem = strstr(request,method[1])) != NULL){
		start = &request[strlen(method[1])];
		if( (end = strstr(start,":")) != NULL){
			start = &request[strlen(method[1])];
			strncpy(rlt,start,end - start);
			rlt[end - start] = '\0';
			return 1;
		}
	}
	else if( (tem = strstr(request,method[2])) != NULL){
		start = &request[strlen(method[2])];
		if( (end = strstr(start,":")) != NULL){
			start = &request[strlen(method[2])];
			strncpy(rlt,start,end - start);
			rlt[end - start] = '\0';
			return 1;
		}
	}
	else
		;
	return 0;
}
// proxy sub function
// it extracts host and port(if there is) from the request , connect to the host returns the socket
// or there is no host,returns -1
int connectHost(char * request,char * service)
{
	char key[] = "Host: ";
	char sep[] = "\r\n";
	char ipstr[16];
	char * pkey = NULL;
	char * psep = NULL;
	char * phost = NULL;//store HOST:www.***.com:***
	char * ptem = NULL;
	struct addrinfo hint,*answer,*curr;
	int clientfd = -1;
	struct sockaddr_in sa_server;
	int ret = 0;
	printf("connctHost:%s\r\n",service);
	if ((pkey = strstr(request,key)) != NULL){
		printf("%s",pkey);
		if ( (psep = strstr(pkey,sep)) != NULL){
			fprintf(stderr,"%d",(psep - pkey - strlen(key) + 1));
			phost = (char *)malloc((psep - pkey - strlen(key) + 1)*sizeof(char));
			if (phost == NULL){
				fprintf(stderr,"malloc error");
				return -1;
			}
			strncpy(phost,&pkey[strlen(key)],psep - pkey - strlen(key));
			phost[psep - pkey - strlen(key)] = '\0';
			
			//use getaddrinfo to get the ip address ip
			bzero(&hint,sizeof(hint));
			hint.ai_family = AF_INET;
			hint.ai_socktype = SOCK_STREAM;
			
			ret = getaddrinfo(phost,service,&hint,&answer);
			if ( ret != 0){
				fprintf(stderr,"getaddrinfo %s\n",gai_strerror(ret));
				free(phost);
				return -1;
			}
			//create socket connecting to server
			if ( (clientfd = socket(AF_INET,SOCK_STREAM,0)) < 0){
				fprintf(stderr,"connectHost failed while creating socket connecting to server %s\n",strerror(errno));
				free(phost);
				return -1;
			}
			//connect to server, returns socket if succeeds
			for ( curr = answer;curr != NULL; curr = curr->ai_next){
				inet_ntop(AF_INET,&(((struct sockaddr_in *)(curr->ai_addr))->sin_addr),ipstr,16);
				fprintf(stderr,"%s %d \r\n",ipstr,ntohs(((struct sockaddr_in *)(curr->ai_addr))->sin_port));
				if ( connect(clientfd,curr->ai_addr,curr->ai_addrlen) < 0)
					continue;
				else{
					fprintf(stderr,"server fd %d\r\n",clientfd);					
					//free(phost);
					
					return clientfd;
				}
			}
			free(phost);
		}
		else
			fprintf(stderr,"connectHost not find Host %s %s",sep,request);
		
	}
	else
		fprintf(stderr,"connectHost not find Host %s %s",key,request);
	
	return -1;			

}


