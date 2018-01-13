microproxy:server1.2.o buffer.o heap.o
	gcc -o microproxy server1.2.o buffer.o heap.o -pthread
server1.2.o:server1.2.c buffer.h heap.h
	gcc -c server1.2.c -pthread
buffer.o:buffer.c buffer.h heap.h
	gcc -c buffer.c
heap.o:heap.c heap.h
	gcc -c heap.c
