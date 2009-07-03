CC=gcc -Wall
LD=libtool --mode=link gcc

CFLAGS:=-std=c99 -march=native -g3 $(CFLAGS)
LFLAGS:=-march=native -g3 -lactivetwo -leegpanel -lbdffileio

all: main

main: main.o
	$(CC) $< $(LFLAGS) -o $@


clean:
	$(RM) main
	$(RM) *.o
