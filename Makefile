CC ?= gcc
CFLAGS += -std=c99 -Wall -Wextra -pedantic -MMD -ggdb
CFLAGS += -Iinclude

TARGETS = f3write f3read
BIN_TARGETS := $(addprefix bin/,$(TARGETS))
EXTRA_TARGETS = f3probe f3brew f3fix
BIN_EXTRAS := $(addprefix bin/,$(EXTRA_TARGETS))

PREFIX = /usr/local
INSTALL = install
LN = ln

ifndef OS
	OS = $(shell uname -s)
endif
ifneq ($(filter $(OS),Linux Darwin),$(OS))
    $(warning Unknown OS '$(OS)', defaulting to Linux)
    OS := Linux
endif

# Platform-specific include and linker flags
PLATFORM_CFLAGS =
PLATFORM_LDFLAGS =

ifeq ($(OS), Linux)
    PLATFORM_DIR = linux
    PLATFORM_CFLAGS +=
    PLATFORM_LDFLAGS +=
endif
ifeq ($(OS), Darwin)
	PLATFORM_DIR = darwin
    ifneq ($(shell command -v brew),)
        ARGP_PREFIX := $(shell brew --prefix)
    else
        ARGP_PREFIX := /usr/local
    endif
    PLATFORM_CFLAGS += -I$(ARGP_PREFIX)/include
    PLATFORM_LDFLAGS += -L$(ARGP_PREFIX)/lib -largp
endif

CFLAGS += $(PLATFORM_CFLAGS)
LDFLAGS += $(PLATFORM_LDFLAGS)

# Common libraries used by all platforms
COMMON_LIBS = -lm

# OS-specific libraries and flags
F3PROBE_LIBS = $(COMMON_LIBS)
F3BREW_LIBS = $(COMMON_LIBS)
F3FIX_LIBS =

ifeq ($(OS), Linux)
    F3PROBE_LIBS += -ludev
    F3BREW_LIBS += -ludev
    F3FIX_LIBS += -lparted
endif
ifeq ($(OS), Darwin)
    F3PROBE_LIBS +=
    F3BREW_LIBS +=
    F3FIX_LIBS +=
endif

# source directories and automatic rules
SRC_DIRS = src/commands src/core src/devices src/platform/$(PLATFORM_DIR)
vpath %.c $(SRC_DIRS)

DEVICE_SRCS := $(wildcard src/devices/*.c)
DEVICE_OBJS := $(DEVICE_SRCS:.c=.o)
PLATFORM_SRCS := $(wildcard src/platform/$(PLATFORM_DIR)/*.c)
PLATFORM_OBJS := $(PLATFORM_SRCS:.c=.o)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

all: $(TARGETS)
extra: $(EXTRA_TARGETS)

docker:
	docker build -f Dockerfile -t f3:latest .

install: all
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -m755 $(BIN_TARGETS) $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/share/man/man1
	$(INSTALL) -m644 f3read.1 $(DESTDIR)$(PREFIX)/share/man/man1
	$(LN) -sf f3read.1 $(DESTDIR)$(PREFIX)/share/man/man1/f3write.1

install-extra: extra
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -m755 $(BIN_EXTRAS) $(DESTDIR)$(PREFIX)/bin

# command targets
f3write: src/commands/f3write.o src/core/utils.o src/core/libflow.o
	$(CC) -o bin/$@ $^ $(LDFLAGS) $(COMMON_LIBS)

f3read: src/commands/f3read.o src/core/utils.o src/core/libflow.o
	$(CC) -o bin/$@ $^ $(LDFLAGS) $(COMMON_LIBS)

f3probe: src/commands/f3probe.o src/core/libutils.o src/core/libdevs.o src/core/libprobe.o $(DEVICE_OBJS) $(PLATFORM_OBJS)
	$(CC) -o bin/$@ $^ $(LDFLAGS) $(F3PROBE_LIBS)

f3brew: src/commands/f3brew.o src/core/libutils.o src/core/libdevs.o $(DEVICE_OBJS) $(PLATFORM_OBJS)
	$(CC) -o bin/$@ $^ $(LDFLAGS) $(F3BREW_LIBS)

f3fix: src/commands/f3fix.o src/core/libutils.o
	$(CC) -o bin/$@ $^ $(LDFLAGS) $(F3FIX_LIBS)

-include *.d

.PHONY: cscope cppcheck clean

cscope:
	cscope -b src/**/*.c include/**/*.h

cppcheck:
	cppcheck --enable=all --suppress=missingIncludeSystem \
	  -Iinclude -Iinclude/devices src include

clean:
	rm -f src/**/**/*.o src/**/**/*.d cscope.out $(BIN_TARGETS) $(BIN_EXTRAS)
