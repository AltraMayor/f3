# --- Basic Configuration ---
CC ?= gcc
# Standard CFLAGS: C99, warnings, pedantic, dependency generation, debug info
# -MMD: Generate dependency file as a side effect
# -MP: Add phony targets for headers to avoid errors if headers are deleted
# -MF $(@:.o=.d): Name the dependency file based on the object file name,
#                 placing it next to the object file in the build directory.
CFLAGS += -std=c99 -Wall -Wextra -pedantic -MMD -MP -MF $(@:.o=.d) -ggdb
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
ifneq ($(filter $(OS),Linux Darwin),)
    # Supported OS - continue
else
    $(warning Unknown OS '$(OS)', defaulting to Linux)
    OS := Linux
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

ifeq ($(OS), Linux)
    PLATFORM_DIR = linux
    PLATFORM_CFLAGS += -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
    PLATFORM_LDFLAGS +=
    F3PROBE_LIBS += -ludev
    F3BREW_LIBS += -ludev
    F3FIX_LIBS += -lparted
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

# Find source files relative to source root
COMMAND_SRCS := $(wildcard src/commands/*.c)
CORE_SRCS := $(wildcard src/core/*.c)
DEVICE_SRCS := $(wildcard src/devices/*.c)
PLATFORM_SRCS := $(wildcard src/platform/$(PLATFORM_DIR)/*.c)

# Map source paths to object paths in OBJ_DIR
ALL_SRCS := $(COMMAND_SRCS) $(CORE_SRCS) $(DEVICE_SRCS) $(PLATFORM_SRCS)
ALL_OBJS := $(addprefix $(OBJ_DIR)/,$(notdir $(ALL_SRCS:.c=.o)))

# Define reusable object lists
DEVICE_OBJS := $(addprefix $(OBJ_DIR)/,$(notdir $(DEVICE_SRCS:.c=.o)))
PLATFORM_OBJS := $(addprefix $(OBJ_DIR)/,$(notdir $(PLATFORM_SRCS:.c=.o)))
LIBDEVS_OBJS := $(addprefix $(OBJ_DIR)/,libdevs.o libutils.o)
LIBDEVICE_PLATFORM_OBJS := $(DEVICE_OBJS) $(PLATFORM_OBJS) $(LIBDEVS_OBJS)
LIBFLOW_OBJS := $(addprefix $(OBJ_DIR)/,libflow.o utils.o)

# --- Pattern Rule for Compilation ---
# This rule tells make how to build an object file at a path like build/obj/src/commands/%.o
# from a source file named %.c (which make finds using vpath).
# The target explicitly defines the output location in OBJ_DIR.
# Make finds the prerequisite %.c using the vpath directive.
$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# --- Main Build Targets ---
all: $(BIN_TARGETS) $(BIN_DIR)
extra: $(BIN_EXTRAS) $(BIN_DIR)

# --- Directory Targets ---
# Explicit targets for the output directories to make them order-only prerequisites
$(BUILD_DIR):
	@mkdir -p $@

$(OBJ_DIR): | $(BUILD_DIR)
	@mkdir -p $@

$(BIN_DIR): | $(BUILD_DIR)
	@mkdir -p $@

# --- Binary Linking Rules (In BIN_DIR) ---
$(BIN_DIR)/f3write: $(OBJ_DIR)/f3write.o $(LIBFLOW_OBJS) | $(BIN_DIR)
	$(CC) -o $@ $^ $(LDFLAGS) $(F3WRITE_LIBS)

$(BIN_DIR)/f3read: $(OBJ_DIR)/f3read.o $(LIBFLOW_OBJS) | $(BIN_DIR)
	$(CC) -o $@ $^ $(LDFLAGS) $(F3READ_LIBS)

$(BIN_DIR)/f3probe: $(OBJ_DIR)/f3probe.o $(OBJ_DIR)/libprobe.o $(LIBDEVICE_PLATFORM_OBJS) | $(BIN_DIR)
	$(CC) -o $@ $^ $(LDFLAGS) $(F3PROBE_LIBS)

$(BIN_DIR)/f3brew: $(OBJ_DIR)/f3brew.o $(LIBDEVICE_PLATFORM_OBJS) | $(BIN_DIR)
	$(CC) -o $@ $^ $(LDFLAGS) $(F3BREW_LIBS)

$(BIN_DIR)/f3fix: $(OBJ_DIR)/f3fix.o $(LIBDEVICE_PLATFORM_OBJS) | $(BIN_DIR)
	$(CC) -o $@ $^ $(LDFLAGS) $(F3FIX_LIBS)

# --- Dependency Inclusion ---
# Include the generated dependency files.
# With -MF $(@:.o=.d), the .d files are created alongside the .o files.
# So we include them from the OBJ_DIR.
-include $(ALL_OBJS:.o=.d)

# --- Installation Targets ---
# Install binaries from the BIN_DIR to the system PREFIX/bin
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

# --- Clean Target ---
# The clean target removes the entire build directory
clean:
	@rm -rf $(BUILD_DIR)
	@rm -f cscope.out

# --- Phony Targets ---
# Declare targets that do not produce files of the same name as phony
.PHONY: all extra docker install install-extra cscope cppcheck clean
# Add directory targets to PHONY to ensure they are always considered
.PHONY: $(BUILD_DIR) $(OBJ_DIR) $(BIN_DIR)
