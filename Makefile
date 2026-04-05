CC = gcc
CFLAGS = -Wall -O2 -D_XOPEN_SOURCE_EXTENDED=1
LDFLAGS = -lncursesw

all: cpuview

cpuview: main.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

main.o: main.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o cpuview
