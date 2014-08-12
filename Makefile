CC ?= gcc
CFLAGS += -std=c99 -Wall -Wextra -pedantic -MMD

TARGETS = f3write f3read
EXPERIMENTAL_TARGETS = f3probe

all: $(TARGETS)
experimental: $(EXPERIMENTAL_TARGETS)

f3write: utils.o f3write.o
	$(CC) -o $@ $^ -lm

f3read: utils.o f3read.o
	$(CC) -o $@ $^

f3probe: f3probe.o
	$(CC) -o $@ $^

-include *.d

PHONY: clean

clean:
	rm -f *.o *.d $(TARGETS) $(EXPERIMENTAL_TARGETS)
