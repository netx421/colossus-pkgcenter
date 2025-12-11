# COLOSSUS System Installer

A small, fast, GTK3-native package manager for Arch Linux that wraps **yay** in a clean GUI.

Designed for Apollo / COLOSSUS builds so non-nerds (kids, spouses, unsuspecting civilians) can
**search, install, remove, and clean packages** without touching the terminal.

---

## ✨ Features

- **GTK3 UI** that follows the system theme (looks like a native system tool)
- **Search packages** via `yay -Ss` (both official repos and AUR)
- Shows:
  - Repository (`core`, `extra`, `community`, `aur`, …)
  - Package name and version
  - Description
  - Whether it’s currently installed
- **Install button**
  - One click, no terminal
  - Uses yay as the normal user
  - Pre-authenticates sudo so yay can call `sudo pacman` internally
- **Remove button**
  - Shows for packages that are actually installed
  - Runs `yay -Rns --noconfirm` to remove the package + unused deps
- **Clean Orphans** button
  - Runs `yay -Yc --noconfirm`
  - Removes packages that nothing depends on anymore
- UI stays **responsive** during installs, uninstalls, and cleanup
  - No more “Application not responding” while yay churns

---

## 🧱 Requirements

This tool is designed for **Arch Linux and Arch-based distros**.

Required packages:

- `gtk3`
- `base-devel`
- `pkgconf` (usually pulled in already, but good to have)
- `yay` (must be installed and working on the system)

The installer script will install the GTK / build deps via `pacman` if needed.
You still need to install **yay** yourself if it’s not already there.

---

## 🛠 Build & Install (one-shot)

From inside the project directory:

```bash
chmod +x install.sh
./install.sh
