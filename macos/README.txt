DeskCode for macOS (Apple Silicon arm64)

Self-contained arm64 binary statically linked against json-c, OpenSSL 3, and
GNU Readline 8; depends only on standard macOS system libraries (libcurl,
libncurses, libc++, libSystem). Intel (x86_64) Macs can build directly from
source following README.md.

One-line installation for the latest release:
  curl -fsSL https://raw.githubusercontent.com/Milorgs/DeskCode/main/install-macos.sh | sh

Manual package use:
  ./desk
or install the extracted binary with:
  sh ./install.sh --setup-path

Open a new terminal, then enter desk.

Select 3 to add your own model. Settings are under ~/.config/desk-agent
(or XDG_CONFIG_HOME). Desktop inspection/actions use System Events (grant
Accessibility permission to your terminal app when prompted).

Uninstall: remove ~/.local/bin/desk and ~/.local/share/desk-agent/installed.
