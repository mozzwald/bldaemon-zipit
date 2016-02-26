# Makefile for bldaemon-zipit
CC?=gcc
CFLAGS=-L. -L.. -O2 --std=c99
LIBS=-lrt -lpthread -lconfuse


all:
	$(CC) $(CFLAGS) -c *.c
	$(CC) $(CFLAGS) *.o -o bldaemon $(LIBS)
	
clean:
	rm -f *.o bldaemon
