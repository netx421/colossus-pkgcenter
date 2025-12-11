
---

## 3️⃣ `install.sh`

Create `install.sh` in the repo root:

```bash
#!/usr/bin/env bash
#
# install.sh — installer for COLOSSUS System Installer
#
# - Installs dependencies via pacman
# - Builds colossus-pkgcenter
# - Installs binary + .desktop (and optional icon)
#
# Arch / Arch-based only.

set -e

APP_NAME="COLOSSUS System Installer"
BIN_NAME="colossus-pkgcenter"
PREFIX="/usr/local"
BIN_DIR="$PREFIX/bin"
DESKTOP_DIR="/usr/share/applications"
ICON_DIR="/usr/share/pixmaps"
DESKTOP_FILE="colossus-pkgcenter.desktop"
ICON_FILE="colossus-pkgcenter.png"   # optional; only installed if present

echo "==> Installing $APP_NAME"

# ───────────────────────────────────────────────
#  Check distro
# ───────────────────────────────────────────────

if ! command -v pacman >/dev/null 2>&1; then
    echo "ERROR: pacman not found. This installer is for Arch / Arch-based distros only."
    exit 1
fi

# ───────────────────────────────────────────────
#  Elevate to root if needed
# ───────────────────────────────────────────────

if [[ $EUID -ne 0 ]]; then
    echo "==> Re-running as root using sudo..."
    exec sudo "$0" "$@"
fi

# ───────────────────────────────────────────────
#  Install dependencies
# ───────────────────────────────────────────────

echo "==> Installing dependencies (gtk3, base-devel, pkgconf)..."
pacman -Syu --needed --noconfirm gtk3 base-devel pkgconf

# We DON'T auto-install yay, but we warn if it's missing.
if ! command -v yay >/dev/null 2>&1; then
    echo
    echo "WARNING: 'yay' is not installed."
    echo "COLOSSUS System Installer uses yay as its backend."
    echo "Please install yay (e.g., from AUR) before using the app."
    echo
fi

# ───────────────────────────────────────────────
#  Build binary
# ───────────────────────────────────────────────

echo "==> Building $BIN_NAME..."

# If a Makefile exists, use it. Otherwise, compile directly with g++.
if [[ -f Makefile ]]; then
    make clean || true
    make
else
    echo "No Makefile found; compiling directly with g++..."
    g++ -std=c++17 -O2 -Wall -Wextra \
        colossus_pkgcenter.cpp \
        -o "$BIN_NAME" \
        $(pkg-config --cflags --libs gtk+-3.0)
fi

if [[ ! -f "$BIN_NAME" ]]; then
    echo "ERROR: build failed; binary '$BIN_NAME' not found."
    exit 1
fi

# ───────────────────────────────────────────────
#  Install binary
# ───────────────────────────────────────────────

echo "==> Installing binary to: $BIN_DIR/$BIN_NAME"
install -Dm755 "$BIN_NAME" "$BIN_DIR/$BIN_NAME"

# ───────────────────────────────────────────────
#  Install .desktop
# ───────────────────────────────────────────────

if [[ -f "$DESKTOP_FILE" ]]; then
    echo "==> Installing desktop file to: $DESKTOP_DIR/$DESKTOP_FILE"
    install -Dm644 "$DESKTOP_FILE" "$DESKTOP_DIR/$DESKTOP_FILE"
else
    echo "WARNING: $DESKTOP_FILE not found; skipping desktop entry install."
fi

# ───────────────────────────────────────────────
#  Install icon (optional)
# ───────────────────────────────────────────────

if [[ -f "$ICON_FILE" ]]; then
    echo "==> Installing icon to: $ICON_DIR/$ICON_FILE"
    install -Dm644 "$ICON_FILE" "$ICON_DIR/$ICON_FILE"
else
    echo "NOTE: No $ICON_FILE found in the current directory."
    echo "      If you create an icon, place it here and re-run install.sh"
fi

# ───────────────────────────────────────────────
#  Finish
# ───────────────────────────────────────────────

echo
echo "==> $APP_NAME installed."
echo "    Binary : $BIN_DIR/$BIN_NAME"
echo "    Desktop: $DESKTOP_DIR/$DESKTOP_FILE"
echo
echo "You should now see \"$APP_NAME\" in your application menu."
echo "Have fun, and try not to install Wine by accident again. 😉"
