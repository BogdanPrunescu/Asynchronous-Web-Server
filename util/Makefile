CC = gcc -g -Wall
	
build: aws.o sock_util.o http_parser.o util.h debug.h
	$(CC) -o aws -I. aws.o sock_util.o http_parser.o util.h debug.h -laio

aws.o: aws.c
	$(CC) -c aws.c 

sock_util.o: sock_util.c sock_util.h
	$(CC) -c -I. sock_util.c sock_util.h

http_parser.o: http_parser.c http_parser.h
	$(CC) -c http_parser.c http_parser.h

.PHONY: clean

clean:
	rm -f *.o aws
