#!/bin/bash
# Install DeskCode (`desk`) inside a Harbor / Terminal-Bench task container.
#
# Runs as root. Downloads the Linux x86_64 release tarball (which contains `desk` and its own
# `install.sh`), installs the runtime libraries it links against and installs the binary
# with `install.sh --prefix "$DESK_PREFIX"`.
#
# Environment (all optional):
#   DESK_RELEASE_URL  tarball URL (default: latest GitHub release asset)
#   DESK_PREFIX       install prefix (default: /usr/local -> /usr/local/bin/desk)
#   DESK_SKIP_DEPS=1  do not touch the system package manager
#
# The release binary is built on Ubuntu 22.04: it needs glibc >= 2.35 and the shared
# libraries libcurl, json-c, OpenSSL 3, GNU Readline 8 and AT-SPI. Alpine/musl images are
# not supported by the prebuilt binary.
set -euo pipefail

DESK_RELEASE_URL=${DESK_RELEASE_URL:-https://github.com/Milorgs/DeskCode/releases/latest/download/desk-agent-linux-x64.tar.gz}
DESK_PREFIX=${DESK_PREFIX:-/usr/local}

log() { printf '[deskcode-install] %s\n' "$*" >&2; }

case "$(uname -m)" in
    x86_64|amd64) ;;
    *) log "unsupported architecture $(uname -m); the DeskCode release is x86_64 only"; exit 1 ;;
esac

if [ -f /etc/alpine-release ]; then
    log "Alpine (musl) detected: the prebuilt glibc binary cannot run here"
    exit 1
fi

apt_install_first() {
    # Install the first available package among the alternatives (t64 renames on newer distros).
    local pkg
    for pkg in "$@"; do
        if apt-get install -y -qq --no-install-recommends "$pkg" >/dev/null 2>&1; then
            return 0
        fi
    done
    log "warning: none of [$*] could be installed"
    return 0
}

install_deps() {
    if [ "${DESK_SKIP_DEPS:-0}" = 1 ]; then
        return
    fi
    if command -v apt-get >/dev/null 2>&1; then
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq >/dev/null
        apt-get install -y -qq --no-install-recommends ca-certificates curl tar gzip >/dev/null
        apt_install_first libcurl4t64 libcurl4
        apt_install_first libjson-c5 libjson-c4
        apt_install_first libssl3t64 libssl3
        apt_install_first libreadline8t64 libreadline8
        apt_install_first libatspi2.0-0t64 libatspi2.0-0
    elif command -v dnf >/dev/null 2>&1; then
        dnf install -y -q ca-certificates curl tar gzip libcurl json-c openssl-libs readline at-spi2-core >/dev/null
    elif command -v yum >/dev/null 2>&1; then
        yum install -y -q ca-certificates curl tar gzip libcurl json-c openssl-libs readline at-spi2-core >/dev/null
    else
        log "no supported package manager found; assuming runtime libraries are present"
    fi
}

download() {
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 3 --connect-timeout 20 "$1" -o "$2"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$2" "$1"
    elif command -v python3 >/dev/null 2>&1; then
        python3 -c 'import sys, urllib.request; urllib.request.urlretrieve(sys.argv[1], sys.argv[2])' "$1" "$2"
    else
        log "need curl, wget or python3 to download DeskCode"
        exit 1
    fi
}

install_deps

tmp=$(mktemp -d /tmp/deskcode-install.XXXXXX)
trap 'rm -rf "$tmp"' EXIT
log "downloading $DESK_RELEASE_URL"
download "$DESK_RELEASE_URL" "$tmp/desk.tar.gz"
tar -xzf "$tmp/desk.tar.gz" -C "$tmp"
pkg=$(find "$tmp" -maxdepth 2 -name install.sh -path '*desk-agent-linux-x64*' | head -n 1)
[ -n "$pkg" ] || { log "release archive does not contain desk-agent-linux-x64/install.sh"; exit 1; }

# install.sh refuses to overwrite a foreign `desk`; a previous DeskCode install is replaced.
HOME=${HOME:-/root} sh "$pkg" --prefix "$DESK_PREFIX"
chmod 755 "$DESK_PREFIX/bin/desk"

if command -v ldd >/dev/null 2>&1; then
    missing=$(ldd "$DESK_PREFIX/bin/desk" 2>&1 | grep -E 'not found|version .* not found' || true)
    if [ -n "$missing" ]; then
        log "desk is missing shared libraries (glibc >= 2.35 required):"
        printf '%s\n' "$missing" >&2
        exit 1
    fi
fi

if [ "$DESK_PREFIX" != /usr/local ] && [ ! -e /usr/local/bin/desk ] && [ -w /usr/local/bin ]; then
    ln -s "$DESK_PREFIX/bin/desk" /usr/local/bin/desk
fi
"$DESK_PREFIX/bin/desk" --version
