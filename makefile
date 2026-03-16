CC=gcc

CFLAGS=-lsqlite3 -lpthread -lssl -lcrypto

all:
	$(CC) server.c database.c simulator.c -o server $(CFLAGS)

run:
	./server

clean:
	rm -f server