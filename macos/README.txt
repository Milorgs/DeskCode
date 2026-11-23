DeskCode for macOS (Universal: Apple Silicon arm64 & Intel x86_64)

Self-contained Universal 2 binary statically linked against json-c, OpenSSL 3,
and GNU Readline 8; depends only on standard macOS system libraries (libcurl,
libncurses, libc++, libSystem).

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
