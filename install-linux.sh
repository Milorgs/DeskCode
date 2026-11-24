#!/bin/sh
set -eu

repo='Milorgs/DeskCode'
case "$(uname -m)" in
    x86_64|amd64) pkg='desk-agent-linux-x64' ;;
    aarch64|arm64) pkg='desk-agent-linux-arm64' ;;
    *) echo "DeskCode Linux packages support x86_64 and arm64 (got $(uname -m))." >&2; exit 1 ;;
esac

asset="https://github.com/$repo/releases/latest/download/$pkg.tar.gz"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/desk-agent-install.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

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
[ -f "$tmp/$pkg/install.sh" ] || { echo 'Invalid DeskCode package.' >&2; exit 1; }
sh "$tmp/$pkg/install.sh" --setup-path
