CC ?= gcc
CFLAGS += -std=c99 -Wall -Wextra -pedantic -MMD -ggdb

TARGETS = f3write f3read
EXPERIMENTAL_TARGETS = f3probe

all: $(TARGETS)
experimental: $(EXPERIMENTAL_TARGETS)

f3write: utils.o f3write.o
	$(CC) -o $@ $^ -lm

f3read: utils.o f3read.o
	$(CC) -o $@ $^

f3probe: libprobe.o f3probe.o
	$(CC) -o $@ $^

-include *.d

PHONY: cscope clean

cscope:
	cscope -b *.c *.h

clean:
	rm -f *.o *.d cscope.out $(TARGETS) $(EXPERIMENTAL_TARGETS)
