# vc root Makefile
# Supports: Linux, macOS, Windows (MSYS2/MinGW)
#
# Usage:
#   make              Build vc, vcd (Linux only), and vcg
#   make vc           Build vc client only
#   make vcg        Build vcg only
#   make install-deps Install required libraries
#   make clean        Remove all build artefacts

CC      = gcc
SRCS    = $(filter-out vctest.c vcetst.c vcd.c, $(wildcard *.c))
OBJS    = $(SRCS:.c=.o)
CFLAGS  = -I. -I../ -I../utils/incs -std=c11 -Wall -Wextra \
          -Wno-unused-parameter -Wno-deprecated-declarations \
          -Wno-format-truncation \
          -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDFLAGS = -L. -L../ -L../utils/libs
PRG     = vc

# ---------- Detect platform ----------------------------------------------
UNAME := $(shell uname -s 2>/dev/null || echo Windows)

ifeq ($(findstring MINGW,$(UNAME)),MINGW)
    PLATFORM = windows
else ifeq ($(findstring MSYS,$(UNAME)),MSYS)
    PLATFORM = windows
else ifeq ($(UNAME),Darwin)
    PLATFORM = macos
else
    PLATFORM = linux
endif

# ---------- Platform-specific settings -----------------------------------
ifeq ($(PLATFORM),windows)
    PRG      = vc.exe
    LIBS     = -lzip -lreadline -lcrypt -lws2_32 -lshlwapi
    CFLAGS  += -DVC_WINDOWS
    # vcd is Linux-only
    VCD_TARGET =
    VCG_EXTRA =
    $(info Platform: Windows (MSYS2/MinGW))

else ifeq ($(PLATFORM),macos)
    # Homebrew prefix detection
    BREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /usr/local)
    CFLAGS  += -I$(BREW_PREFIX)/include -DVC_MACOS
    LDFLAGS += -L$(BREW_PREFIX)/lib
    LIBS     = -lzip -lreadline -lncurses
    # crypt is in libcrypt on macOS (Homebrew) or built-in on older macOS
    LIBS    += $(shell brew --prefix libxcrypt 2>/dev/null && echo -lcrypt || echo "")
    VCD_TARGET = vcd
    $(info Platform: macOS ($(BREW_PREFIX)))

else
    # Linux
    LIBS     = -lzip -lcurses -lreadline -lcrypt
    VCD_TARGET = vcd
    $(info Platform: Linux)
endif

# ---------- Targets ------------------------------------------------------
.PHONY: all vc vcd vcg clean test install-deps help

all: vc $(VCD_TARGET) vcg

vc: $(OBJS)
	@echo "Linking vc..."
	$(CC) -o $(PRG) $(OBJS) $(LDFLAGS) $(LIBS)
	@echo "Built $(PRG)."

%.o: %.c vc.h
	$(CC) $(CFLAGS) -c -o $@ $<

vcd: vcd.c vcd.h vcSha256.c vcSha256.h
	@echo "Building vcd (Linux only)..."
	$(CC) -o vcd vcd.c vcSha256.c -lcrypt
	@echo "Built vcd."

vcg:
	@if [ -d vcg ]; then \
	    echo "Building vcg..."; \
	    $(MAKE) --no-print-directory -C vcg PLATFORM=$(PLATFORM); \
	    echo "Built vcg."; \
	else \
	    echo "vcg/ not found — skipping."; \
	fi

vcg-install:
	$(MAKE) --no-print-directory -C vcg install

vcg-clean:
	@test -d vcg && $(MAKE) --no-print-directory -C vcg clean || true

clean: vcg-clean
	rm -f $(OBJS) $(PRG) vc.exe vctest vcd

test: vc vctest
	./vctest $(shell pwd)/$(PRG)

vctest: vctest.c
	$(CC) $(CFLAGS) -o vctest vctest.c

install-deps:
ifeq ($(PLATFORM),windows)
	@echo "Run in MSYS2 terminal:"
	@echo "  pacman -S mingw-w64-x86_64-libzip"
	@echo "  pacman -S mingw-w64-x86_64-readline"
	@echo "  pacman -S mingw-w64-x86_64-gtk3"
	@echo "  pacman -S mingw-w64-x86_64-pkg-config"
else ifeq ($(PLATFORM),macos)
	brew install libzip readline gtk+3 pkg-config
else
	@bash setup.sh
endif
	@test -d vcg && $(MAKE) --no-print-directory -C vcg deps PLATFORM=$(PLATFORM) || true

help:
	@echo "Targets:"
	@echo "  make              Build vc, vcd (Linux), and vcg"
	@echo "  make vc           Build vc client only"
	@echo "  make vcd          Build vcd daemon (Linux only)"
	@echo "  make vcg        Build vcg GTK3 GUI"
	@echo "  make vcg-install Install vcg to /usr/local/bin"
	@echo "  make install-deps Install required libraries"
	@echo "  make test         Run the test suite"
	@echo "  make clean        Remove all build artefacts"
	@echo ""
	@echo "Detected platform: $(PLATFORM)"

