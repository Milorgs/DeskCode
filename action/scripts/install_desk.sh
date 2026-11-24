#!/usr/bin/env bash
# Download and install a DeskCode release for the current runner.
# Usage: install_desk.sh <version|latest> <dest-dir>
# Prints the absolute path of the installed `desk` executable as the last line of stdout.
# All progress messages go to stderr.
set -euo pipefail

version=${1:-latest}
dest=${2:?usage: install_desk.sh <version|latest> <dest-dir>}
repo=${DESK_ACTION_RELEASE_REPO:-Milorgs/DeskCode}
os=${RUNNER_OS:-$(uname -s)}
arch=${RUNNER_ARCH:-$(uname -m)}

err() { printf '::error::%s\n' "$*" >&2; exit 1; }

case "$os" in
    Linux)
        case "$arch" in
            X64|x86_64|amd64) asset=desk-agent-linux-x64.tar.gz; dir=desk-agent-linux-x64 ;;
            ARM64|aarch64|arm64) asset=desk-agent-linux-arm64.tar.gz; dir=desk-agent-linux-arm64 ;;
            *) err "DeskCode does not publish a Linux build for $arch (only x64 and arm64)." ;;
        esac ;;
    macOS|Darwin) asset=desk-agent-macos-universal.tar.gz; dir=desk-agent-macos-universal ;;
    Windows|MINGW*|MSYS*|CYGWIN*)
        case "$arch" in
            X64|x86_64|amd64) asset=desk-agent-windows-x64.zip; dir= ;;
            ARM64|aarch64|arm64) asset=desk-agent-windows-arm64.zip; dir= ;;
            *) err "DeskCode does not publish a Windows build for $arch (only x64 and arm64)." ;;
        esac ;;
    *) err "Unsupported runner OS: $os" ;;
esac

case "$version" in
    ''|latest) base="https://github.com/$repo/releases/latest/download" ;;
    [0-9]*) base="https://github.com/$repo/releases/download/v$version" ;;
    *) base="https://github.com/$repo/releases/download/$version" ;;
esac

mkdir -p "$dest"
echo "Downloading $base/$asset" >&2
curl -fsSL --retry 3 --retry-delay 2 -o "$dest/$asset" "$base/$asset" \
    || err "Failed to download $base/$asset (check the 'version' input)."

# Verify the checksum when the release publishes SHA256SUMS.txt.
if curl -fsSL --retry 3 -o "$dest/SHA256SUMS.txt" "$base/SHA256SUMS.txt" 2>/dev/null; then
    expected=$(awk -v a="$asset" '{ f=$2; sub(/^\*/, "", f); n=split(f, p, "/"); if (p[n] == a) { print $1; exit } }' "$dest/SHA256SUMS.txt")
    if [ -n "$expected" ]; then
        if command -v sha256sum >/dev/null 2>&1; then
            actual=$(sha256sum "$dest/$asset" | awk '{print $1}')
        else
            actual=$(shasum -a 256 "$dest/$asset" | awk '{print $1}')
        fi
        [ "$actual" = "$expected" ] || err "Checksum mismatch for $asset"
        echo "Checksum OK ($asset)" >&2
    else
        echo "::warning::$asset is not listed in SHA256SUMS.txt; skipping checksum verification." >&2
    fi
fi

if [ -z "$dir" ]; then
    mkdir -p "$dest/windows"
    if command -v unzip >/dev/null 2>&1; then
        unzip -q -o "$dest/$asset" -d "$dest/windows" >&2
    else
        python3 -m zipfile -e "$dest/$asset" "$dest/windows" >&2 2>/dev/null || python -m zipfile -e "$dest/$asset" "$dest/windows" >&2
    fi
    exe=$(find "$dest/windows" -name desk.exe -type f | head -n 1)
    [ -n "$exe" ] || err "desk.exe not found in $asset"
    echo "$exe"
    exit 0
fi

tar -xzf "$dest/$asset" -C "$dest"
sh "$dest/$dir/install.sh" --prefix "$dest/prefix" >&2
[ -x "$dest/prefix/bin/desk" ] || err "install.sh did not produce $dest/prefix/bin/desk"
echo "$dest/prefix/bin/desk"
