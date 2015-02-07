CC ?= gcc
CFLAGS += -std=c99 -Wall -Wextra -pedantic -MMD -ggdb

TARGETS = f3write f3read
EXPERIMENTAL_TARGETS = f3probe f3brew f3fix

PREFIX = /usr/local
INSTALL = install
LN = ln

all: $(TARGETS)
experimental: $(EXPERIMENTAL_TARGETS)

install: all
	$(INSTALL) -oroot -groot -m755 $(TARGETS) $(PREFIX)/bin
	$(INSTALL) -oroot -groot -m644 f3read.1 $(PREFIX)/share/man/man1
	$(LN) -sf f3read.1 $(PREFIX)/share/man/man1/f3write.1

install-experimental: experimental
	$(INSTALL) -oroot -groot -m755 $(EXPERIMENTAL_TARGETS) $(PREFIX)/bin

f3write: utils.o f3write.o
	$(CC) -o $@ $^ -lm

f3read: utils.o f3read.o
	$(CC) -o $@ $^

f3probe: libutils.o libdevs.o libprobe.o utils.o f3probe.o
	$(CC) -o $@ $^ -lm -ludev

f3brew: libutils.o libdevs.o utils.o f3brew.o
	$(CC) -o $@ $^ -lm -ludev

f3fix: utils.o f3fix.o
	$(CC) -o $@ $^ -lparted

-include *.d

PHONY: cscope clean

cscope:
	cscope -b *.c *.h

clean:
	rm -f *.o *.d cscope.out $(TARGETS) $(EXPERIMENTAL_TARGETS)
