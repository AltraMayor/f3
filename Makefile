CC = gcc
CFLAGS = -Wall -MMD

TARGETS = f3write f3read

all: $(TARGETS)

f3write: utils.o f3write.o
	$(CC) -o $@ $^ -lm

f3read: utils.o f3read.o
	$(CC) -o $@ $^

-include *.d

PHONY: clean

clean:
	rm -f *.o $(TARGETS)
