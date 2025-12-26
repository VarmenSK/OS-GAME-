all: server client

server: server.c
	gcc server.c -o server -pthread

client: client.c
	gcc client.c -o client -pthread

clean:
	rm -f server client *.o *.log
