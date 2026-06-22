#!/bin/bash
# setup.sh — install all libraries required to build vc
#
# Supports:
#   Debian / Ubuntu       (apt)
#   Fedora / RHEL / CentOS (dnf / yum)
#   Arch Linux             (pacman)
#   macOS                  (brew)
#
# Run with:  sudo ./setup.sh
# Or:        bash setup.sh       (will sudo internally when needed)

set -e

# ---------------------------------------------------------------------------
# Detect package manager
# ---------------------------------------------------------------------------
if command -v apt-get &>/dev/null; then
    PKG_MGR="apt"
elif command -v dnf &>/dev/null; then
    PKG_MGR="dnf"
elif command -v yum &>/dev/null; then
    PKG_MGR="yum"
elif command -v pacman &>/dev/null; then
    PKG_MGR="pacman"
elif command -v brew &>/dev/null; then
    PKG_MGR="brew"
else
    echo "ERROR: No supported package manager found."
    echo "       Please install the following libraries manually:"
    echo "         libzip, libreadline, libncurses, libcrypt"
    exit 1
fi

echo "Detected package manager: $PKG_MGR"
echo

# ---------------------------------------------------------------------------
# Require root for system package managers (not brew)
# ---------------------------------------------------------------------------
if [[ "$PKG_MGR" != "brew" && $EUID -ne 0 ]]; then
    echo "Re-running with sudo..."
    exec sudo bash "$0" "$@"
fi

# ---------------------------------------------------------------------------
# Install packages
# ---------------------------------------------------------------------------
case "$PKG_MGR" in

    apt)
        echo "Updating package list..."
        apt-get update -qq

        echo "Installing required libraries..."
        apt-get install -y \
            gcc \
            make \
            libzip-dev \
            libreadline-dev \
            libncurses-dev \
            libcrypt-dev
        ;;

    dnf|yum)
        echo "Installing required libraries..."
        $PKG_MGR install -y \
            gcc \
            make \
            libzip-devel \
            readline-devel \
            ncurses-devel \
            libxcrypt-devel
        ;;

    pacman)
        echo "Installing required libraries..."
        pacman -Sy --noconfirm \
            gcc \
            make \
            libzip \
            readline \
            ncurses \
            libxcrypt
        ;;

    brew)
        echo "Installing required libraries (macOS)..."
        brew install \
            libzip \
            readline \
            ncurses \
            openssl
        echo
        echo "NOTE: On macOS you may need to set LDFLAGS and CPPFLAGS."
        echo "      Homebrew will show the exact paths after installation."
        ;;
esac

# ---------------------------------------------------------------------------
# Verify
# ---------------------------------------------------------------------------
echo
echo "Verifying installations..."
MISSING=0

check_lib() {
    local name="$1"
    local header="$2"
    if [[ -f "$header" ]]; then
        echo "  OK  $name  ($header)"
    else
        echo "  MISSING  $name"
        MISSING=$((MISSING + 1))
    fi
}

check_lib "libzip"      "$(find /usr -name 'zip.h'      2>/dev/null | head -1)"
check_lib "libreadline" "$(find /usr -name 'readline.h' 2>/dev/null | head -1)"
check_lib "libncurses"  "$(find /usr -name 'ncurses.h'  2>/dev/null | head -1)"
check_lib "libcrypt"    "$(find /usr -name 'crypt.h'    2>/dev/null | head -1)"

echo
if [[ $MISSING -eq 0 ]]; then
    echo "All libraries installed successfully."
    echo "You can now build vc with:  make"
else
    echo "WARNING: $MISSING library/libraries could not be verified."
    echo "         The build may fail — check the output above."
    exit 1
fi
