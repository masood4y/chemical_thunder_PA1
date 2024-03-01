CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g

all: rsend rrecv

rsend: sender.o
	$(CC) $(CFLAGS) -o rsend sender.o

rrecv: receiver.o
	$(CC) $(CFLAGS) -o rrecv receiver.o

sender.o: sender.c our_protocol.h
	$(CC) $(CFLAGS) -c sender.c

receiver.o: receiver_bytes.c our_protocol.h
	$(CC) $(CFLAGS) -c receiver.c

clean:
	rm -f rsend rrecv *.o
 