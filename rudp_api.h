#ifndef RUDP_API_H
#define	RUDP_API_H
#include "rudp.h"
#define RUDP_MAXPKTSIZE 1000	/* Number of data bytes that can sent in a
				 * packet, RUDP header not included */
#define RUDP_MAXSESSION 200

/*
 * Event types for callback notifications
 */

typedef enum {
	RUDP_EVENT_TIMEOUT, 
	RUDP_EVENT_CLOSED,
} rudp_event_t; 

/*
 * States for a rudp session
 */
typedef enum {
	RUDP_SESSION_CLOSED,
	RUDP_SESSION_OPENED,
	RUDP_SESSION_SYN,
	RUDP_SESSION_DAT,
	RUDP_SESSION_FIN,
} rudp_session_state_t;

/*
 * RUDP socket handle
 */


//typedef void *rudp_socket_t;
typedef struct _rudp_socket_t* rudp_socket_t;
struct _rudp_socket_t {
	int sockfd;
	int (*recv_handler)(rudp_socket_t, struct sockaddr_in*, char*, int);
	int (*event_handler)(rudp_socket_t, rudp_event_t, struct sockaddr_in*);
	int nsessions;
};
/*
 * RUDP Datagram 
 */

struct _rudp_dgram_t {
	struct rudp_hdr hdr;
	char data[RUDP_MAXPKTSIZE];
	int data_len;
	int retrans;
	struct _rudp_dgram_t* next;
};
typedef struct _rudp_dgram_t* rudp_dgram_t;

/*
 * RUDP session handle
 */
struct _rudp_session_t {
	rudp_socket_t rsock;/* rudp socket associated with this session */
	int session_id;     /* unique Id per session */
	struct sockaddr_in target; /* Remote host IP address and port */
	rudp_session_state_t state; /* holds the current state of the session */
	rudp_dgram_t head; /* head pointer of the buffer holding session data */
	rudp_dgram_t tail; /* tail pointer holds the FIN Packet */
	rudp_dgram_t wstart; /* start of the window */
	rudp_dgram_t wend; /* end of the window */
	int wsize; /* Window size at any given moment */
};
typedef struct _rudp_session_t* rudp_session_t; 


/*
 * Prototypes
 */

/* 
 * Socket creation 
 */
rudp_socket_t rudp_socket(int port);

/* 
 * Socket termination
 */
int rudp_close(rudp_socket_t rsocket);

/* 
 * Send a datagram 
 */
int rudp_sendto(rudp_socket_t rsocket, void* data, int len, 
		struct sockaddr_in* to);

/* 
 * Register callback function for packet receiption 
 * Note: data and len arguments to callback function 
 * are only valid during the call to the handler
 */
int rudp_recvfrom_handler(rudp_socket_t rsocket, 
			  int (*handler)(rudp_socket_t, 
					 struct sockaddr_in *, 
					 char *, int));
/*
 * Register callback handler for event notifications
 */
int rudp_event_handler(rudp_socket_t rsocket, 
		       int (*handler)(rudp_socket_t, 
				      rudp_event_t, 
				      struct sockaddr_in *));
#endif /* RUDP_API_H */
