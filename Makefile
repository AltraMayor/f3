CC ?= gcc
CFLAGS += -std=c17 -Wall -Wextra -pedantic -MMD -ggdb

BUILD_DIR = build
SRC_DIR = src

TARGETS = $(BUILD_DIR)/f3write $(BUILD_DIR)/f3read
EXTRA_TARGETS = $(BUILD_DIR)/f3probe $(BUILD_DIR)/f3brew $(BUILD_DIR)/f3fix

PREFIX = /usr/local
INSTALL = install
LN = ln
UNLINK = unlink

ifndef OS
	OS = $(shell uname -s)
endif
ifneq ($(OS), Linux)
	ARGP = /usr/local
	ifeq ($(OS), Darwin)
		ifneq ($(shell command -v brew),)
			# Apple Silicon Homebrew lives under /opt/homebrew; Intel under
			# /usr/local. `brew --prefix` resolves either. Prefer the
			# argp-standalone keg path when present (it is more robust if the
			# formula is keg-only), else fall back to the Homebrew prefix.
			ARGP = $(shell brew --prefix argp-standalone 2>/dev/null || brew --prefix)
		endif
	endif
	CFLAGS += -I$(ARGP)/include
	LDFLAGS += -L$(ARGP)/lib -largp
endif

# libudev is Linux-only; f3probe's Darwin backend does not use it.
ifeq ($(OS), Linux)
	UDEV_LIBS = -ludev
else
	UDEV_LIBS =
endif

# Among the "extra" tools, only f3probe is ported to macOS: f3brew needs
# libudev and f3fix needs libparted, both out of scope on Darwin.
ifeq ($(OS), Darwin)
	EXTRA_TARGETS = $(BUILD_DIR)/f3probe
endif

all: $(TARGETS)
extra: $(EXTRA_TARGETS)

docker:
	docker build -f Dockerfile -t f3:latest .

install: all
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -m755 $(TARGETS) $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/share/man/man1
	$(INSTALL) -m644 man/f3read.1 $(DESTDIR)$(PREFIX)/share/man/man1
	$(LN) -sf f3read.1 $(DESTDIR)$(PREFIX)/share/man/man1/f3write.1

install-extra: extra
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -m755 $(EXTRA_TARGETS) $(DESTDIR)$(PREFIX)/bin

uninstall: uninstall-extra
	cd $(DESTDIR)$(PREFIX)/bin ; rm $(notdir $(TARGETS))
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/f3read.1
	$(UNLINK) $(DESTDIR)$(PREFIX)/share/man/man1/f3write.1

uninstall-extra:
	cd $(DESTDIR)$(PREFIX)/bin ; rm $(notdir $(EXTRA_TARGETS))

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/f3write: $(BUILD_DIR)/libutils.o $(BUILD_DIR)/libfile.o $(BUILD_DIR)/libflow.o $(BUILD_DIR)/f3write.o
	$(CC) -o $@ $^ $(LDFLAGS) -lm

$(BUILD_DIR)/f3read: $(BUILD_DIR)/libutils.o $(BUILD_DIR)/libfile.o $(BUILD_DIR)/libflow.o $(BUILD_DIR)/f3read.o
	$(CC) -o $@ $^ $(LDFLAGS) -lm

$(BUILD_DIR)/f3probe: $(BUILD_DIR)/libutils.o $(BUILD_DIR)/libflow.o $(BUILD_DIR)/libdevs.o $(BUILD_DIR)/libprobe.o $(BUILD_DIR)/f3probe.o
	$(CC) -o $@ $^ $(LDFLAGS) -lm $(UDEV_LIBS)

$(BUILD_DIR)/f3brew: $(BUILD_DIR)/libutils.o $(BUILD_DIR)/libflow.o $(BUILD_DIR)/libdevs.o $(BUILD_DIR)/f3brew.o
	$(CC) -o $@ $^ $(LDFLAGS) -lm $(UDEV_LIBS)

$(BUILD_DIR)/f3fix: $(BUILD_DIR)/libutils.o $(BUILD_DIR)/f3fix.o
	$(CC) -o $@ $^ $(LDFLAGS) -lparted

-include $(BUILD_DIR)/*.d

.PHONY: cscope clean uninstall uninstall-extra

cscope:
	cscope -b $(SRC_DIR)/*.c $(SRC_DIR)/*.h

clean:
	rm -rf $(BUILD_DIR) cscope.out
