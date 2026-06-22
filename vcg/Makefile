# vcg Makefile
# Supports: Linux, macOS (Homebrew), Windows (MSYS2/MinGW)
#
# Usage:
#   make              Build vcg
#   make install      Install to /usr/local/bin (Linux/Mac) or ~/bin (Windows)
#   make clean        Remove build artefacts
#   make deps         Install GTK3 dev libraries

TARGET = vcg

SRCS = vcg.c \
       vcgConfig.c \
       vcgOutput.c \
       vcgTree.c \
       vcgCallbacks.c \
       vcgPrefs.c \
       vcgUi.c

OBJS = $(SRCS:.c=.o)
CC   = gcc

# ---------- Detect platform ----------------------------------------------
UNAME    := $(shell uname -s 2>/dev/null || echo Windows)
PLATFORM ?= linux

ifeq ($(findstring MINGW,$(UNAME)),MINGW)
    PLATFORM = windows
else ifeq ($(findstring MSYS,$(UNAME)),MSYS)
    PLATFORM = windows
else ifeq ($(UNAME),Darwin)
    PLATFORM = macos
else
    PLATFORM = linux
endif

# ---------- Platform-specific GTK3 flags ---------------------------------
ifeq ($(PLATFORM),windows)
    TARGET   = vcg.exe
    CFLAGS   = $(shell pkg-config --cflags gtk+-3.0) -I.. -std=c11 -Wall \
               -Wno-unused-parameter -Wno-deprecated-declarations
    LIBS     = $(shell pkg-config --libs gtk+-3.0)
    PREFIX   = $(HOME)/bin
    $(info vcg platform: Windows (MSYS2))

else ifeq ($(PLATFORM),macos)
    BREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /usr/local)
    CFLAGS   = $(shell PKG_CONFIG_PATH=$(BREW_PREFIX)/lib/pkgconfig \
                       pkg-config --cflags gtk+-3.0) \
               -I.. -std=c11 -Wall -Wno-unused-parameter \
               -Wno-deprecated-declarations
    LIBS     = $(shell PKG_CONFIG_PATH=$(BREW_PREFIX)/lib/pkgconfig \
                       pkg-config --libs gtk+-3.0)
    PREFIX   = /usr/local
    $(info vcg platform: macOS ($(BREW_PREFIX)))

else
    CFLAGS   = $(shell pkg-config --cflags gtk+-3.0) -I.. -std=c11 -Wall \
               -Wno-unused-parameter -Wno-deprecated-declarations
    LIBS     = $(shell pkg-config --libs gtk+-3.0)
    PREFIX   = /usr/local
    $(info vcg platform: Linux)
endif

.PHONY: all install uninstall clean deps

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)
	@echo "Built $(TARGET)."

%.o: %.c vcg.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	install -m 755 $(TARGET) $(PREFIX)/$(TARGET)
	@echo "Installed to $(PREFIX)/$(TARGET)"

uninstall:
	rm -f $(PREFIX)/$(TARGET)

clean:
	rm -f $(TARGET) vcg.exe $(OBJS)

deps:
ifeq ($(PLATFORM),windows)
	@echo "Run in MSYS2 MinGW64 terminal:"
	@echo "  pacman -S mingw-w64-x86_64-gtk3"
	@echo "  pacman -S mingw-w64-x86_64-pkg-config"
else ifeq ($(PLATFORM),macos)
	brew install gtk+3 pkg-config
else
	@if command -v apt-get >/dev/null 2>&1; then \
	    sudo apt-get install -y libgtk-3-dev pkg-config; \
	elif command -v dnf >/dev/null 2>&1; then \
	    sudo dnf install -y gtk3-devel pkgconfig; \
	elif command -v pacman >/dev/null 2>&1; then \
	    sudo pacman -S --noconfirm gtk3 pkgconf; \
	else \
	    echo "Install GTK3 dev libraries manually."; \
	fi
endif

