CC ?= gcc
CFLAGS += -std=c99 -Wall -Wextra -pedantic -MMD -ggdb

TARGETS = f3write f3read
EXTRA_TARGETS = f3probe f3brew f3fix

PREFIX = /usr/local
INSTALL = install
LN = ln

all: $(TARGETS)
extra: $(EXTRA_TARGETS)

install: all
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -oroot -groot -m755 $(TARGETS) $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/share/man/man1
	$(INSTALL) -oroot -groot -m644 f3read.1 $(DESTDIR)$(PREFIX)/share/man/man1
	$(LN) -sf f3read.1 $(DESTDIR)$(PREFIX)/share/man/man1/f3write.1

install-extra: extra
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -oroot -groot -m755 $(EXTRA_TARGETS) $(DESTDIR)$(PREFIX)/bin

f3write: utils.o f3write.o
	$(CC) -o $@ $^ $(LDFLAGS) -lm

f3read: utils.o f3read.o
	$(CC) -o $@ $^ $(LDFLAGS)

f3probe: libutils.o libdevs.o libprobe.o f3probe.o
	$(CC) -o $@ $^ $(LDFLAGS) -lm -ludev

f3brew: libutils.o libdevs.o f3brew.o
	$(CC) -o $@ $^ $(LDFLAGS) -lm -ludev

f3fix: libutils.o f3fix.o
	$(CC) -o $@ $^ $(LDFLAGS) -lparted

-include *.d

PHONY: cscope clean

cscope:
	cscope -b *.c *.h

clean:
	rm -f *.o *.d cscope.out $(TARGETS) $(EXTRA_TARGETS)
