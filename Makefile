CC = gcc
CFLAGS = -Wall -g

all: OWR

OWR: main.c rede.c
	$(CC) $(CFLAGS) -o OWR main.c rede.c

clean:
	rm -f OWR