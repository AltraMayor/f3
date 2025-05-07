# --- Basic Configuration ---
CC ?= gcc
CFLAGS += -std=c99 -Wall -Wextra -pedantic -ggdb
CFLAGS += -Iinclude

# --- Target Definitions ---
TARGETS = f3write f3read
EXTRA_TARGETS = f3probe f3brew f3fix

# --- Installation Paths and Tools ---
PREFIX = /usr/local
INSTALL = install
LN = ln

# --- OS Detection ---
OS ?= $(shell uname -s)
ifeq ($(OS),Linux)
    PLATFORM := linux
else ifeq ($(OS),Darwin)
    PLATFORM := darwin
    # Check Xcode command line tools
    ifeq (,$(shell xcode-select -p))
        $(error Xcode command line tools are not installed)
    endif
else
    $(warning Unknown OS '$(OS)', defaulting to Linux)
    PLATFORM := linux
endif

# --- Platform-Specific Flags and Libraries Definitions ---
PLATFORM_CFLAGS =
PLATFORM_LDFLAGS =

# Target specific libraries and flags
COMMON_LIBS = -lm
F3WRITE_LIBS = $(COMMON_LIBS)
F3READ_LIBS = $(COMMON_LIBS)
F3PROBE_LIBS = $(COMMON_LIBS)
F3BREW_LIBS = $(COMMON_LIBS)
F3FIX_LIBS =

ifeq ($(PLATFORM), linux)
    PLATFORM_DIR = linux
    PLATFORM_CFLAGS += -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
    PLATFORM_LDFLAGS +=
    F3PROBE_LIBS += -ludev
    F3BREW_LIBS += -ludev
    F3FIX_LIBS += -lparted
endif
ifeq ($(PLATFORM), darwin)
    PLATFORM_DIR = darwin
    ifneq ($(shell command -v brew),)
        ARGP_PREFIX := $(shell brew --prefix)
    else
        ARGP_PREFIX := /usr/local
    endif
    PLATFORM_CFLAGS += -I$(ARGP_PREFIX)/include
    PLATFORM_LDFLAGS += -L$(ARGP_PREFIX)/lib -largp
    PLATFORM_LDFLAGS += -framework DiskArbitration -framework CoreFoundation
endif

CFLAGS += $(PLATFORM_CFLAGS)
LDFLAGS += $(PLATFORM_LDFLAGS)

# --- Output Directory Setup ---
BUILD_DIR := build
BIN_DIR := $(BUILD_DIR)/bin
OBJ_DIR := $(BUILD_DIR)/obj

BIN_TARGETS := $(addprefix $(BIN_DIR)/,$(TARGETS))
BIN_EXTRAS := $(addprefix $(BIN_DIR)/,$(EXTRA_TARGETS))
# Source directories and automatic search rules
SRC_DIRS = src/commands src/core src/devices src/platform/$(PLATFORM_DIR)
vpath %.c $(SRC_DIRS)
# Find source files relative to source root and map them to object paths in OBJ_DIR
ALL_SRCS := $(wildcard $(addsuffix /*.c, $(SRC_DIRS)))
ALL_OBJS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(ALL_SRCS))
# Device and platform-specific object lists
DEVICE_OBJS   := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(wildcard src/devices/*.c))
PLATFORM_OBJS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(wildcard src/platform/$(PLATFORM_DIR)/*.c))
# Reusable object lists
LIBDEVS_OBJS := $(patsubst src/%.c,$(OBJ_DIR)/%.o, src/core/libdevs.c src/core/libutils.c)
LIBFLOW_OBJS := $(patsubst src/%.c,$(OBJ_DIR)/%.o, src/core/libflow.c src/core/utils.c)
LIBDEVICE_PLATFORM_OBJS := $(DEVICE_OBJS) $(PLATFORM_OBJS) $(LIBDEVS_OBJS)

# --- Dependency Inclusion ---
DEPFLAGS = -MMD -MT $@ -MP -MF $(@:.o=.d)
-include $(ALL_OBJS:.o=.d)

# --- Pattern Rule for Compilation ---
$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# --- Main Build Targets ---
all: $(BIN_TARGETS)
extra: $(BIN_EXTRAS)

# --- Directory Targets ---
$(BUILD_DIR): ; @mkdir -p $@
$(BIN_DIR): | $(BUILD_DIR) ; @mkdir -p $@
$(OBJ_DIR): | $(BUILD_DIR) ; @mkdir -p $@

# --- Binary Linking Rules ---
$(BIN_DIR)/f3write: $(OBJ_DIR)/commands/f3write.o $(LIBFLOW_OBJS) | $(BIN_DIR)
	$(CC) -o $@ $^ $(LDFLAGS) $(F3WRITE_LIBS)

$(BIN_DIR)/f3read: $(OBJ_DIR)/commands/f3read.o $(LIBFLOW_OBJS) | $(BIN_DIR)
	$(CC) -o $@ $^ $(LDFLAGS) $(F3READ_LIBS)

$(BIN_DIR)/f3probe: $(OBJ_DIR)/commands/f3probe.o $(OBJ_DIR)/core/libprobe.o $(LIBDEVICE_PLATFORM_OBJS) | $(BIN_DIR)
	$(CC) -o $@ $^ $(LDFLAGS) $(F3PROBE_LIBS)

$(BIN_DIR)/f3brew: $(OBJ_DIR)/commands/f3brew.o $(LIBDEVICE_PLATFORM_OBJS) | $(BIN_DIR)
	$(CC) -o $@ $^ $(LDFLAGS) $(F3BREW_LIBS)

$(BIN_DIR)/f3fix: $(OBJ_DIR)/commands/f3fix.o $(LIBDEVICE_PLATFORM_OBJS) | $(BIN_DIR)
	$(CC) -o $@ $^ $(LDFLAGS) $(F3FIX_LIBS)

# --- Installation Targets ---
install: all
	@echo "Installing binaries from $(BIN_DIR) to $(DESTDIR)$(PREFIX)/bin"
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -m755 $(BIN_TARGETS) $(DESTDIR)$(PREFIX)/bin
	@echo "Installing man pages to $(DESTDIR)$(PREFIX)/share/man/man1"
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/share/man/man1
	$(INSTALL) -m644 f3read.1 $(DESTDIR)$(PREFIX)/share/man/man1
	$(LN) -sf f3read.1 $(DESTDIR)$(PREFIX)/share/man/man1/f3write.1

install-extra: extra
	@echo "Installing extra binaries from $(BIN_DIR) to $(DESTDIR)$(PREFIX)/bin"
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -m755 $(BIN_EXTRAS) $(DESTDIR)$(PREFIX)/bin

# --- Development Helper Targets ---
cscope:
	cscope -b src/**/*.c include/**/*.h

cppcheck:
	cppcheck --enable=all --suppress=missingIncludeSystem \
	  -Iinclude -Iinclude/devices src include

docker:
	docker build -f Dockerfile -t f3:latest .

# --- Cleanup and Maintenance Targets ---
clean:
	@rm -rf $(BUILD_DIR)
	@rm -f cscope.out

.PHONY: all extra docker install install-extra cscope cppcheck clean
