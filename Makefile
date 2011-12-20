linux:
	gcc -Wall -c utils.c
	gcc -Wall -c f3write.c
	gcc -Wall -c f3read.c
	gcc -o f3write utils.o f3write.o -lm
	gcc -o f3read utils.o f3read.o

mac:
	gcc -Wall -DAPPLE_MAC -c utils.c
	gcc -Wall -DAPPLE_MAC -c f3write.c
	gcc -Wall -DAPPLE_MAC -c f3read.c
	gcc -o f3write utils.o f3write.o -lm
	gcc -o f3read utils.o f3read.o

clean:
	rm -f *.o f3write f3read
