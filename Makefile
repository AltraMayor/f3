# --- Basic Configuration ---
CC ?= gcc
CFLAGS += -std=c99 -Wall -Wextra -pedantic -ggdb -Iinclude/f3

TARGETS = f3write f3read
EXTRA_TARGETS = f3probe f3brew f3fix

PREFIX = /usr/local
INSTALL = install
LN = ln
UNLINK = unlink

# --- OS Detection ---
ifndef OS
	OS = $(shell uname -s)
endif
ifneq ($(OS), Linux)
	ARGP = /usr/local
	ifeq ($(OS), Darwin)
		ifneq ($(shell command -v brew),)
			ARGP = $(shell brew --prefix)
		endif
	endif
	CFLAGS += -I$(ARGP)/include
	LDFLAGS += -L$(ARGP)/lib -largp
endif

# --- Output Directory Setup ---
BUILD_DIR := build
BIN_DIR := $(BUILD_DIR)/bin
OBJ_DIR := $(BUILD_DIR)/obj

BIN_TARGETS := $(addprefix $(BIN_DIR)/,$(TARGETS))
BIN_EXTRAS := $(addprefix $(BIN_DIR)/,$(EXTRA_TARGETS))

# Source directories and automatic search rules
SRC_DIRS = src/f3 src/f3-extra
vpath %.c $(SRC_DIRS)

# Find source files relative to source root and map them to object paths in OBJ_DIR
TARGET_SRCS := $(wildcard $(addsuffix /*.c, $(SRC_DIRS)))
TARGET_OBJS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(TARGET_SRCS))

# Reusable object lists
F3_LIB_OBJS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(wildcard src/f3/lib/*.c))
F3_EXTRA_LIB_OBJS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(wildcard src/f3-extra/lib/*.c))

ALL_OBJS := $(TARGET_OBJS) $(F3_LIB_OBJS) $(F3_EXTRA_LIB_OBJS)

# --- Dependency Inclusion ---
DEPFLAGS = -MMD -MT $@ -MP -MF $(@:.o=.d)
ALL_DEPS := $(ALL_OBJS:.o=.d)
-include $(ALL_DEPS)

# --- Pattern Rule for Compilation ---
$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# --- Main Build Targets ---
all: $(BIN_TARGETS)
extra: $(BIN_EXTRAS)

docker:
	docker build -f Dockerfile -t f3:latest .

# --- Installation Targets ---
install: all
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -m755 $(BIN_TARGETS) $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/share/man/man1
	$(INSTALL) -m644 f3read.1 $(DESTDIR)$(PREFIX)/share/man/man1
	$(LN) -sf f3read.1 $(DESTDIR)$(PREFIX)/share/man/man1/f3write.1

install-extra: extra
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -m755 $(BIN_EXTRAS) $(DESTDIR)$(PREFIX)/bin

uninstall: uninstall-extra
	cd $(DESTDIR)$(PREFIX)/bin ; rm $(TARGETS)
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/f3read.1
	$(UNLINK) $(DESTDIR)$(PREFIX)/share/man/man1/f3write.1

uninstall-extra:
	cd $(DESTDIR)$(PREFIX)/bin ; rm $(EXTRA_TARGETS)

# --- Directory Targets ---
$(BUILD_DIR): ; @mkdir -p $@
$(BIN_DIR): | $(BUILD_DIR) ; @mkdir -p $@
$(OBJ_DIR): | $(BUILD_DIR) ; @mkdir -p $@

# --- Binary Linking Rules ---
$(BIN_DIR)/f3write: $(OBJ_DIR)/f3/f3write.o $(F3_LIB_OBJS) | $(BIN_DIR)
	$(CC) -o $@ $^ $(LDFLAGS) -lm

$(BIN_DIR)/f3read: $(OBJ_DIR)/f3/f3read.o $(F3_LIB_OBJS) | $(BIN_DIR)
	$(CC) -o $@ $^ $(LDFLAGS) -lm

$(BIN_DIR)/f3probe: $(OBJ_DIR)/f3-extra/f3probe.o $(F3_EXTRA_LIB_OBJS) | $(BIN_DIR)
	$(CC) -o $@ $^ $(LDFLAGS) -lm -ludev

$(BIN_DIR)/f3brew: $(OBJ_DIR)/f3-extra/f3brew.o $(F3_EXTRA_LIB_OBJS) | $(BIN_DIR)
	$(CC) -o $@ $^ $(LDFLAGS) -lm -ludev

$(BIN_DIR)/f3fix: $(OBJ_DIR)/f3-extra/f3fix.o $(F3_EXTRA_LIB_OBJS) | $(BIN_DIR)
	$(CC) -o $@ $^ $(LDFLAGS) -lparted

# --- Cleanup and Maintenance Targets ---
.PHONY: all extra docker install install-extra uninstall uninstall-extra cscope clean

cscope:
	cscope -b src/**/*.c include/**/*.h

clean:
	@rm -rf $(BUILD_DIR)
	@rm -f cscope.out
