#!/bin/sh
set -eu

repo='Milorgs/DeskCode'
pkg_name='desk-agent-macos-universal'
asset="https://github.com/$repo/releases/latest/download/$pkg_name.tar.gz"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/desk-agent-install.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

[ "$(uname -s)" = "Darwin" ] || { echo 'This installer is for macOS.' >&2; exit 1; }

case "$(uname -m)" in
    arm64|aarch64|x86_64) ;;
    *) echo "Unsupported macOS architecture: $(uname -m)" >&2; exit 1 ;;
esac

download() {
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 3 --connect-timeout 15 --proto '=https' --tlsv1.2 "$1" -o "$2"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$2" "$1"
    else echo 'Install curl or wget first.' >&2; exit 1
    fi
}

command -v tar >/dev/null 2>&1 || { echo 'Install tar first.' >&2; exit 1; }
download "$asset" "$tmp/desk-agent.tar.gz"
tar -xzf "$tmp/desk-agent.tar.gz" -C "$tmp"
[ -f "$tmp/$pkg_name/install.sh" ] || { echo 'Invalid DeskCode package.' >&2; exit 1; }
sh "$tmp/$pkg_name/install.sh" --setup-path
