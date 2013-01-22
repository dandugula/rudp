#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "event.h"
#include "rudp.h"
#include "rudp_api.h"
u_int32_t recv_seq_no[RUDP_MAXSESSION];
int recv_session = 0;
rudp_session_t rudp_sessions[RUDP_MAXSESSION];
int nsessions = 0;
int all_close = 0;
int drop = 0;
int rudp_ack_time_cb(int fd, void *arg);
int rudp_recv_cb(int fd, void *arg);
int rudp_send_ack(rudp_socket_t rsock, int i, struct sockaddr_in* to);
int rudp_send_dgram(int pkt_type, rudp_session_t rsession);
rudp_session_t rudp_get_session(rudp_socket_t rsock, struct sockaddr_in* addr);
int rudp_register_session(rudp_session_t rsession);
int rudp_enqueue_data(char* data, int len, rudp_session_t rsession);
int rudp_free_session(rudp_session_t rs);

/* 
 * rudp_socket: Create a RUDP socket. 
 * May use a random port by setting port to zero. 
 */

rudp_socket_t rudp_socket(int port) {
	/* create a SOCK_DGRAM and fill in the session attr for
	   a rudp socket */
	rudp_socket_t rsock;
	struct sockaddr_in local_addr;
#ifdef DEBL
	printf("rudp_socket:...\n");
#endif /* DEB */
	rsock = (rudp_socket_t) malloc (sizeof (struct _rudp_socket_t));
	if(rsock == NULL) return NULL;
	rsock->event_handler = NULL;
	rsock->recv_handler = NULL;
	rsock->nsessions = 0;
	if((rsock->sockfd = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
		perror("socket:");
		return NULL;
	}
	local_addr.sin_family = AF_INET;
	local_addr.sin_port = htons(port);
	/* Use any local address */
	local_addr.sin_addr.s_addr = htonl(INADDR_ANY); 
	/* Socket will be bind to any random port if port number is 0 */
	if((bind(rsock->sockfd, (struct sockaddr*)&local_addr, sizeof(local_addr))) < 0) {
		perror("bind:");
		return NULL;
	}
	/* Register a recv callback with the event handler */	
	if(event_fd(rsock->sockfd, rudp_recv_cb, rsock,
		"Rudp Recv Callback") < 0){
		return NULL;
	}
	return rsock;
	//return NULL;
}

/* 
 *rudp_close: Close socket 
 */ 

int rudp_close(rudp_socket_t rsocket)
{
	rudp_session_t rsessions[30];
	rudp_dgram_t rpackt;
	int len = 0;
	int i;
	for(i = 0; i < nsessions; ++i) {
		if((rudp_sessions[i] != NULL) &&
		(rudp_sessions[i]->rsock->sockfd == rsocket->sockfd)) {
			rsessions[len++] = rudp_sessions[i];
		}
	}
	/* Listening socket*/
	if(len == 0) {
		event_fd_delete(rudp_recv_cb, rsocket);
		close(rsocket->sockfd);
		if(rsocket->event_handler != NULL) {
			rsocket->event_handler(rsocket, 
						RUDP_EVENT_CLOSED, NULL);
		}
		free(rsocket);
	}
	/* Sending socket */
	for(i = 0; i < len; ++i) {
		rsessions[i]->tail = (rudp_dgram_t)malloc(sizeof
					(struct _rudp_dgram_t));
		if(rsessions[i]->tail == NULL) return -1;
		memset(rsessions[i]->tail, 0, sizeof(rsessions[i]->tail));
		rsessions[i]->tail->hdr.version = 1;
		rsessions[i]->tail->hdr.type = RUDP_FIN;
		rpackt = rsessions[i]->head;
		while(rpackt->next != NULL) rpackt = rpackt->next;
		rsessions[i]->tail->hdr.seqno = rpackt->hdr.seqno + 1;
		rudp_send_dgram(RUDP_FIN, rsessions[i]);
	}
	/* Free the rudp socket and close the SOCK_DGRAM */
	return 0;
}

/* 
 *rudp_recvfrom_handler: Register receive callback function 
 */ 

int rudp_recvfrom_handler(rudp_socket_t rsocket, 
			  int (*handler)(rudp_socket_t, struct sockaddr_in *, 
					 char *, int))
{
	if(rsocket == NULL || handler == NULL) return -1;
	rsocket->recv_handler = handler;
	return 0;
}

/* 
 *rudp_event_handler: Register event handler callback function 
 */ 
int rudp_event_handler(rudp_socket_t rsocket, 
		       int (*handler)(rudp_socket_t, rudp_event_t, 
				      struct sockaddr_in *)) 
{
	if(rsocket == NULL || handler == NULL) return -1;
	rsocket->event_handler = handler;
	return 0;
}


/* 
 * rudp_sendto: Send a block of data to the receiver. 
 */

int rudp_sendto(rudp_socket_t rsocket, void* data, int len, struct sockaddr_in* to) 
{
	rudp_session_t rsession = NULL;
	/* Check if session already exists for the to address */
	rsession = rudp_get_session(rsocket, to);
	/* If no session then create one */
	if(NULL == rsession) {
		rsession = (rudp_session_t)malloc(
				sizeof(struct _rudp_session_t));
		if(rsession == NULL) return -1;
		rsession->session_id = nsessions;
		rsession->rsock = rsocket;
		/* Increment the number of sessions on this socket */
		++rsocket->nsessions;
		//rsession->sessionid = get_next_id();
		rsession->target.sin_family = to->sin_family;
		rsession->target.sin_port = to->sin_port;
		memcpy(&(rsession->target.sin_addr), 
		       &(to->sin_addr), sizeof(struct in_addr));
		rsession->state = RUDP_SESSION_OPENED;
		rsession->head = NULL;	
		rsession->wstart = NULL;
		rsession->wend = NULL;
		//rsession->last_seq = get_init_seq();
		if(rudp_register_session(rsession) < 0) {
			return -1;
		}
		if(rudp_send_dgram(RUDP_SYN, rsession) < 0 ) {
			return -1;
		}
	}
	if(rudp_enqueue_data(data, len, rsession) < 0) {
		return  -1;
	}
	/* if State = OPEN  then send SYN */
	/* Register t/o function for SYN with event handler */
	/* enque data into a buffer */
	return 0;
}
int rudp_ack_time_cb(int fd, void *arg)
{
	rudp_session_t rsession = (rudp_session_t) arg;
	if(rsession->state == RUDP_SESSION_OPENED) {
		if(rsession->head->retrans < RUDP_MAXRETRANS) {
			return rudp_send_dgram(RUDP_SYN, rsession);
			//return 0;
		}
	}else if(rsession->state == RUDP_SESSION_SYN) {
		if(rsession->wstart->retrans < RUDP_MAXRETRANS) {
			rsession->wsize = 0;
			return rudp_send_dgram(RUDP_DATA, rsession);
			//return 0;
		}
	}else if(rsession->state == RUDP_SESSION_DAT) {
		if(rsession->tail->retrans < RUDP_MAXRETRANS) {
			return rudp_send_dgram(RUDP_FIN, rsession);
			//return 0;
		}
	}
	if(rsession->rsock->event_handler != NULL) {
		rsession->rsock->event_handler(rsession->rsock, 
				RUDP_EVENT_TIMEOUT, &(rsession->target));
	}
	return 0;
}
int rudp_recv_cb(int fd, void *arg)
{
	rudp_socket_t rsock = (rudp_socket_t)arg;
	rudp_session_t rsession = NULL;
	rudp_dgram_t rudp_pkt;
	struct sockaddr_in from;
	int i;
	size_t from_len;
	int ret;
	char buf[sizeof(struct _rudp_dgram_t)];
	memset((char*)&from, 0, sizeof(struct sockaddr_in));
	from_len = sizeof(from);
	ret = recvfrom(fd, buf, sizeof(struct _rudp_dgram_t), 0, 
		(struct sockaddr*)&from, &from_len);
	if(ret < 0) {
		perror("recvfrom:");
		return -1;
	}

	//drop = 1;
	rudp_pkt = (rudp_dgram_t) buf;
#ifdef DROP
	++drop;
	if((drop % 5 == 0) &&
	   (rudp_pkt->hdr.type == RUDP_DATA)) { 
		printf("Dropping.. %d\n", drop); return 0;
	}
#endif /* DROP */
	if(rudp_pkt->hdr.type == RUDP_ACK) {
		rsession = rudp_get_session(rsock, &from);
		if(NULL == rsession) {
			printf("rudp_recv_cb: No session found\n");
			return -1;
		}
	}
	/* Session not correct return immediatedly */
	
#ifdef DEB
	printf("Packet type: %d\n\
		Packet version: %d\n\
		Packet SeqNo.: %d\n", rudp_pkt->hdr.type, rudp_pkt->hdr.version,
		rudp_pkt->hdr.seqno);
#endif /* DEB */
	switch(rudp_pkt->hdr.type) {
	case RUDP_DATA:
		for(i = 0; i < recv_session; ++i) {
		/* Check if correct seq no. */
		if((SEQ_EQ(recv_seq_no[i], rudp_pkt->hdr.seqno)) &&
		  (rsock->recv_handler != NULL)) {
			rsock->recv_handler(rsock, &from, rudp_pkt->data,
				    rudp_pkt->data_len);
			rudp_send_ack(rsock, i, &from);
			break;
		}
		}
		/* Else drop the packet */
		break;
	case RUDP_ACK:
		//if(rsession->last_seq + 1 != rudp_pkt->hdr.seqno) break;
		if(rsession->state == RUDP_SESSION_OPENED) {
			rsession->state = RUDP_SESSION_SYN;
			event_timeout_delete(rudp_ack_time_cb, rsession);
			rsession->wstart = rsession->head->next;
			rudp_send_dgram(RUDP_DATA, rsession);
			rsession->wsize = 0;
		} else if(rsession->state == RUDP_SESSION_SYN) {
			while(SEQ_LT(rsession->wstart->hdr.seqno,
					rudp_pkt->hdr.seqno)) {
				rsession->wstart = rsession->wstart->next;
				if(NULL == rsession->wstart){
					event_timeout_delete(rudp_ack_time_cb, 
							rsession);
					rsession->state = RUDP_SESSION_DAT;
					rudp_send_dgram(RUDP_FIN, rsession);
					break;
				}
				++rsession->wsize;	
			}
			//printf("Window Size: %d\n", rsession->wsize);
			if((rsession->wsize == RUDP_WINDOW) &&
			    (NULL != rsession->wstart)) {
				event_timeout_delete(rudp_ack_time_cb, 
							rsession);
				rsession->wsize = 0;
				rudp_send_dgram(RUDP_DATA, rsession);
			}
		}else if(rsession->state == RUDP_SESSION_DAT) {
			rudp_socket_t r;
			rsession->state = RUDP_SESSION_FIN;
			event_timeout_delete(rudp_ack_time_cb, rsession);
			r = rsession->rsock;
			rudp_free_session(rsession);
#ifdef DEBL
			printf("nsessions: %d\n", r->nsessions);
#endif /* DEBL */
			/* Last session on this socket */
			if(r->nsessions == 1) {
				event_fd_delete(rudp_recv_cb, r);
				r->event_handler(r, RUDP_EVENT_CLOSED, NULL);
				close(r->sockfd);
				free(r);
			} else {
				/* Decrement the sessions on this socket */
				--r->nsessions;
			}
		}
		break;
	case RUDP_SYN:
		recv_seq_no[recv_session] = rudp_pkt->hdr.seqno;
		rudp_send_ack(rsock, recv_session++, &from);
		all_close = recv_session;
		break;
	case RUDP_FIN:
		for(i = 0; i < recv_session; ++i) {
		if(SEQ_EQ(recv_seq_no[i], rudp_pkt->hdr.seqno)){ 
			rudp_send_ack(rsock, i, &from);
			//recv_seq_no[i] = 0;
			--all_close;
			break;
		}
		}
		/* Close the socket on the reciever when all the files
		   are recieved */
		if(!all_close) rudp_close(rsock);
		break;
	}
	return 0;
	
}
int rudp_send_ack(rudp_socket_t rsock, int i, struct sockaddr_in* to)
{
	rudp_dgram_t rudp_pkt;
	++recv_seq_no[i];
	int ret;
	rudp_pkt = (rudp_dgram_t)malloc(sizeof
					(struct _rudp_dgram_t));
	if(rudp_pkt == NULL) return -1;
	rudp_pkt->hdr.version = 1;
	rudp_pkt->hdr.type = RUDP_ACK;
	rudp_pkt->hdr.seqno = recv_seq_no[i];
	ret = sendto(rsock->sockfd, (void*)rudp_pkt,
		sizeof(struct _rudp_dgram_t), 0, 
		(struct sockaddr*)to,
		sizeof(struct sockaddr) );
	if(ret < 0) {
		perror("sendto:");
		return -1;
	}
	return 0;

}
int rudp_send_dgram(int pkt_type, rudp_session_t rsession)
{
	int i;
	struct timeval tv;
	int ret;
	gettimeofday(&tv, NULL);
	tv.tv_sec += (time_t)RUDP_TIMEOUT/1000;
	switch(pkt_type) {
	case RUDP_DATA:
		if(RUDP_SESSION_SYN != rsession->state) {
			printf("rudp_send_dgram(DATA): invalid session state\n");
			return -1;
		}
		rsession->wend = rsession->wstart;
		for(i = 0; i < RUDP_WINDOW; ++i) {
			if(NULL == rsession->wend) {
			  break;
			}
			ret = sendto(rsession->rsock->sockfd, 
			(void*)rsession->wend,
			sizeof(struct _rudp_dgram_t),0, 
			(struct sockaddr*)&(rsession->target),
			sizeof(struct sockaddr) );
			if(ret < 0) {
				perror("sendto:");
				return -1;
			}
			rsession->wend->retrans++;
			rsession->wend = rsession->wend->next;
		}
		event_timeout(tv, rudp_ack_time_cb, rsession, "DATA TIMEOUT");
		break;	
	case RUDP_SYN:
		if(RUDP_SESSION_OPENED != rsession->state) {
			printf("rudp_send_dgram(SYN): invalid session state\n");
			return -1;
		}
		if(rsession->head == NULL) {
			rsession->head = (rudp_dgram_t)malloc(sizeof
					(struct _rudp_dgram_t));
			if(rsession->head == NULL) return -1;
			rsession->head->hdr.version = 1;
			rsession->head->hdr.type = RUDP_SYN;
			rsession->head->hdr.seqno = (u_int32_t)rand();
			rsession->head->data_len = 0;
			rsession->head->next = NULL;
		}
		ret = sendto(rsession->rsock->sockfd, (void*)rsession->head,
			sizeof(struct _rudp_dgram_t), 0, 
			(struct sockaddr*)&(rsession->target),
			sizeof(struct sockaddr) );
		if(ret < 0) {
			perror("sendto:");
			return -1;
		}
		rsession->head->retrans++;
		event_timeout(tv, rudp_ack_time_cb, rsession, "SYN TIMEOUT");
		break;
	case RUDP_FIN:
		if((RUDP_SESSION_DAT == rsession->state) && 
		   (rsession->tail != NULL)) {
			ret = sendto(rsession->rsock->sockfd, 
				(void*)rsession->tail,
			sizeof(struct _rudp_dgram_t), 0, 
			(struct sockaddr*)&(rsession->target),
			sizeof(struct sockaddr) );
			if(ret < 0) {
				perror("sendto:");	
				return -1;
			}
			rsession->tail->retrans++;
			event_timeout(tv, rudp_ack_time_cb,
					rsession,"FIN TIMEOUT");		
		}
		break;
	}
	return 0;
}
rudp_session_t rudp_get_session(rudp_socket_t rsock, struct sockaddr_in* addr)
{
	int i;
	for(i = 0; i < nsessions; ++i) {
#ifdef DEB
		printf("i = %d nsessions = %d\n rudp_session: %x", i, 
			nsessions, rudp_sessions[i]);
#endif /* DEB */
		if((rudp_sessions[i] != NULL) &&
		   (rsock->sockfd == rudp_sessions[i]->rsock->sockfd) && 
		   (rudp_sessions[i]->target.sin_addr.s_addr == 
		    addr->sin_addr.s_addr) && 
		   (rudp_sessions[i]->target.sin_port == addr->sin_port)) {
#ifdef DEBL
		   printf("Match...\n");
		   printf("rsock->fd: %d\n", rsock->sockfd);
		   printf("port: %d\n", addr->sin_port);
#endif /*DEBL*/
		   return rudp_sessions[i];
		}
	}
	return NULL;
} 
int rudp_register_session(rudp_session_t rsession)
{
	if(nsessions >= RUDP_MAXSESSION) {
		return -1;
	}
	rudp_sessions[nsessions++] = rsession;
#ifdef DEBL
	printf("nsessions: %d\n", nsessions);
#endif /* DEBL */
	return 0;
} 
int rudp_enqueue_data(char* data, int len, rudp_session_t rsession)
{
	rudp_dgram_t start, prev;
	start = rsession->head;
	if(rsession->head != NULL) {
		while(start != NULL) {
			prev = start;
			start = start->next;
		}
		start = (rudp_dgram_t)malloc(sizeof(struct _rudp_dgram_t));
		if(start == NULL) return -1;
		start->hdr.version = 1;
		start->hdr.type = RUDP_DATA;
		start->hdr.seqno = prev->hdr.seqno + 1;
		memcpy(start->data, data, len);
		start->data_len = len;			
		start->retrans = 0;
		prev->next = start;
		start->next = NULL;
	}	
	return 0;
} 
int rudp_free_session(rudp_session_t rs)
{
	rudp_dgram_t cur, prev = NULL;
	cur = rs->head;
	while(cur != NULL) {
		prev = cur;
		cur = cur->next;
		free(prev);
	}
	rudp_sessions[rs->session_id] = NULL;
	free(rs);
	return 0;
}
/* t/o for SYN */
/* -if retransmission is less than max send SYN */
/* register this function again with event handler */
/* -else return error */

/* recv callback */
/* -if PACKT_TYPE = ACK then change to state = SYN */
/* deregister t/o for SYN */
/* send data window */
/* -if PACKT_TYPE = ACKm (m < n) */
/* -if PACKT_TYPE = ACKn then chage to state = DATA */
/* deregister t/o for DATA */

