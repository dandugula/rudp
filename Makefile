CC = gcc
CFLAGS = -g -Wall

all: vs_send vs_recv

vs_send: vs_send.o rudp.o event.o
	$(CC) $(CFLAGS) $^ -o $@

vs_recv: vs_recv.o rudp.o event.o
	$(CC) $(CFLAGS) $^ -o $@

vs_send.o vs_recv.o rudp.o: rudp.h rudp_api.h event.h

event.c: event.h

clean:
	/bin/rm -f vs_send vs_recv *.o
