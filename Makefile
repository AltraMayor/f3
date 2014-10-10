CC ?= gcc
CFLAGS += -std=c99 -Wall -Wextra -pedantic -MMD -ggdb

TARGETS = f3write f3read
EXPERIMENTAL_TARGETS = f3probe f3brew

all: $(TARGETS)
experimental: $(EXPERIMENTAL_TARGETS)

f3write: utils.o f3write.o
	$(CC) -o $@ $^ -lm

f3read: utils.o f3read.o
	$(CC) -o $@ $^

f3probe: libutils.o libdevs.o libprobe.o utils.o f3probe.o
	$(CC) -o $@ $^ -lm -ludev

f3brew: libutils.o libdevs.o f3brew.o
	$(CC) -o $@ $^ -lm -ludev

-include *.d

PHONY: cscope clean

cscope:
	cscope -b *.c *.h

clean:
	rm -f *.o *.d cscope.out $(TARGETS) $(EXPERIMENTAL_TARGETS)
