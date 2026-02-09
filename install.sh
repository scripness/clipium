#!/usr/bin/env bash
set -euo pipefail

# Clipium — Universal Install Script
# Detects your distro, installs dependencies, builds, and installs clipium.

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

info()  { echo -e "${BLUE}[*]${NC} $*"; }
ok()    { echo -e "${GREEN}[+]${NC} $*"; }
warn()  { echo -e "${YELLOW}[!]${NC} $*"; }
error() { echo -e "${RED}[-]${NC} $*"; exit 1; }

# --- Platform detection ---

detect_os() {
    if [[ "$OSTYPE" == "linux-gnu"* ]]; then
        echo "linux"
    elif [[ "$OSTYPE" == "darwin"* ]]; then
        echo "macos"
    else
        echo "unknown"
    fi
}

detect_distro() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        echo "${ID:-unknown}"
    elif command -v lsb_release &>/dev/null; then
        lsb_release -si | tr '[:upper:]' '[:lower:]'
    else
        echo "unknown"
    fi
}

OS=$(detect_os)
DISTRO=""
if [ "$OS" = "linux" ]; then
    DISTRO=$(detect_distro)
fi

info "Detected: OS=$OS, Distro=$DISTRO"

# --- Check for Wayland ---

check_wayland() {
    if [ "$OS" = "macos" ]; then
        warn "macOS detected. Clipium is designed for Wayland/GNOME."
        warn "It will not work on macOS. Proceeding for build only."
        return
    fi
    if [ -z "${WAYLAND_DISPLAY:-}" ] && [ -z "${XDG_SESSION_TYPE:-}" ]; then
        warn "Wayland session not detected. Clipium requires Wayland."
    elif [ "${XDG_SESSION_TYPE:-}" = "x11" ]; then
        warn "X11 session detected. Clipium requires Wayland for full functionality."
    fi
}

check_wayland

# --- Install dependencies ---

install_deps_fedora() {
    info "Installing dependencies via dnf..."
    sudo dnf install -y gcc make pkg-config \
        gtk4-devel libadwaita-devel gtk4-layer-shell-devel sqlite-devel \
        wl-clipboard
}

install_deps_ubuntu() {
    info "Installing dependencies via apt..."
    sudo apt-get update
    sudo apt-get install -y gcc make pkg-config \
        libgtk-4-dev libadwaita-1-dev libgtk4-layer-shell-dev libsqlite3-dev \
        wl-clipboard
}

install_deps_debian() {
    install_deps_ubuntu
}

install_deps_arch() {
    info "Installing dependencies via pacman..."
    sudo pacman -S --needed --noconfirm gcc make pkgconf \
        gtk4 libadwaita gtk4-layer-shell sqlite \
        wl-clipboard
}

install_deps_opensuse() {
    info "Installing dependencies via zypper..."
    sudo zypper install -y gcc make pkg-config \
        gtk4-devel libadwaita-devel gtk4-layer-shell-devel sqlite3-devel \
        wl-clipboard
}

install_deps_void() {
    info "Installing dependencies via xbps..."
    sudo xbps-install -Sy gcc make pkg-config \
        gtk4-devel libadwaita-devel gtk4-layer-shell-devel sqlite-devel \
        wl-clipboard
}

install_deps_alpine() {
    info "Installing dependencies via apk..."
    sudo apk add gcc make musl-dev pkgconf \
        gtk4.0-dev libadwaita-dev gtk4-layer-shell-dev sqlite-dev \
        wl-clipboard
}

install_deps_gentoo() {
    info "Installing dependencies via emerge..."
    sudo emerge --noreplace dev-util/pkgconfig \
        x11-libs/gtk+:4 gui-libs/libadwaita dev-db/sqlite \
        gui-apps/wl-clipboard
    warn "gtk4-layer-shell may need to be installed from an overlay."
}

install_deps_nixos() {
    info "For NixOS, add to your configuration.nix or use nix-shell."
    warn "Attempting nix-shell approach..."
    cat << 'NIXEOF'

To build on NixOS, run:
  nix-shell -p gcc gnumake pkg-config gtk4 libadwaita gtk4-layer-shell sqlite wl-clipboard

Then inside the shell:
  make && make install

NIXEOF
}

install_deps_macos() {
    if ! command -v brew &>/dev/null; then
        error "Homebrew not found. Install it from https://brew.sh"
    fi
    info "Installing dependencies via Homebrew..."
    warn "Note: gtk4-layer-shell is not available on macOS."
    warn "Clipium is a Wayland-only application and will not fully work on macOS."
    brew install gtk4 libadwaita sqlite pkg-config
}

install_deps() {
    if [ "$OS" = "macos" ]; then
        install_deps_macos
        return
    fi

    case "$DISTRO" in
        fedora|rhel|centos|rocky|alma)
            install_deps_fedora ;;
        ubuntu|pop|linuxmint|elementary|zorin)
            install_deps_ubuntu ;;
        debian|raspbian)
            install_deps_debian ;;
        arch|manjaro|endeavouros|garuda)
            install_deps_arch ;;
        opensuse*|sles)
            install_deps_opensuse ;;
        void)
            install_deps_void ;;
        alpine)
            install_deps_alpine ;;
        gentoo)
            install_deps_gentoo ;;
        nixos)
            install_deps_nixos
            return ;;
        *)
            warn "Unknown distro: $DISTRO"
            warn "You need: gcc, make, pkg-config, gtk4-devel, libadwaita-devel,"
            warn "          gtk4-layer-shell-devel, sqlite-devel, wl-clipboard"
            echo ""
            read -rp "Continue with build anyway? [y/N] " answer
            if [[ ! "$answer" =~ ^[Yy] ]]; then
                exit 1
            fi
            ;;
    esac
}

# --- Check if deps are already present ---

check_deps() {
    local missing=0
    for pkg in gtk4 libadwaita-1 gtk4-layer-shell-0 sqlite3; do
        if ! pkg-config --exists "$pkg" 2>/dev/null; then
            missing=1
            break
        fi
    done

    if ! command -v gcc &>/dev/null; then
        missing=1
    fi

    return $missing
}

# --- Build ---

build() {
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    cd "$SCRIPT_DIR"

    info "Building clipium..."
    make clean 2>/dev/null || true
    make

    if [ ! -f clipium ]; then
        error "Build failed — binary not produced"
    fi

    local size
    size=$(stat --format=%s clipium 2>/dev/null || stat -f%z clipium 2>/dev/null)
    ok "Build successful! Binary: $(du -h clipium | cut -f1) ($size bytes)"
}

# --- Install ---

do_install() {
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    cd "$SCRIPT_DIR"

    info "Installing clipium to ~/.local/bin/..."
    make install

    # Check if ~/.local/bin is in PATH
    if [[ ":$PATH:" != *":$HOME/.local/bin:"* ]]; then
        warn "$HOME/.local/bin is not in your PATH."
        warn "Add this to your ~/.bashrc or ~/.zshrc:"
        echo '  export PATH="$HOME/.local/bin:$PATH"'
    fi

    ok "Clipium installed successfully!"
    echo ""
    info "Quick start:"
    echo "  clipium &          # Start the daemon"
    echo "  clipium show       # Show the popup"
    echo "  clipium status     # Check status"
    echo "  make keybinding    # Set up Shift+Space shortcut"
    echo ""
    info "To auto-start on login, the desktop file has been installed."
    info "Log out and back in, or run 'clipium &' now."
}

# --- Main ---

main() {
    echo ""
    echo "  Clipium — Clipboard Manager for Wayland/GNOME"
    echo "  ============================================="
    echo ""

    if check_deps; then
        ok "All build dependencies already installed"
    else
        info "Some dependencies are missing"
        install_deps
    fi

    build
    do_install
}

main "$@"
