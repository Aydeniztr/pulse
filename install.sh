#!/bin/bash
set -e

REPO_URL="https://github.com/yourusername/cmon.git" # TODO: Update with your real URL
TMP_DIR=$(mktemp -d)

# Cleanup on exit
trap "rm -rf $TMP_DIR" EXIT

# Detect Package Manager
if [ -f /etc/debian_version ]; then
    echo "Detected Debian/Ubuntu-based system. Installing dependencies..."
    sudo apt update && sudo apt install -y build-essential libncursesw5-dev git
elif [ -f /etc/fedora-release ] || [ -f /etc/redhat-release ]; then
    echo "Detected Fedora/RHEL-based system. Installing dependencies..."
    sudo dnf install -y gcc make ncurses-devel git
elif [ -f /etc/arch-release ]; then
    echo "Detected Arch Linux-based system. Installing dependencies..."
    sudo pacman -S --noconfirm base-devel ncurses git
else
    echo "Warning: Unsupported OS. Please ensure gcc, make, git, and ncurses-devel are installed manually."
fi

# Clone and Build in Temp
echo "Cloning cmon into temporary directory..."
git clone --depth 1 "$REPO_URL" "$TMP_DIR"
cd "$TMP_DIR"

echo "Building cmon from source..."
make

# Install to /usr/local/bin
echo "Installing to /usr/local/bin/cmon..."
sudo mv cmon /usr/local/bin/

echo "----------------------------------------"
echo "✅ cmon has been installed successfully!"
echo "Type 'cmon' to start monitoring."
echo "----------------------------------------"
