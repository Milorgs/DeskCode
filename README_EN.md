# DeskCode

**Native C++20 cross-platform terminal & desktop AI coding assistant** — combining verified atomic file editing, Linux Landlock-sandboxed shell execution, multimodal desktop accessibility automation, native OpenAI & Anthropic streaming, and MCP tools.

[![Build and tests](https://github.com/Milorgs/DeskCode/actions/workflows/test.yml/badge.svg)](https://github.com/Milorgs/DeskCode/actions/workflows/test.yml)
[![Latest Release](https://img.shields.io/github/v/release/Milorgs/DeskCode?label=release)](https://github.com/Milorgs/DeskCode/releases)
[![Standard](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](CMakeLists.txt)
[![Platforms](https://img.shields.io/badge/platform-Linux%20x64%2FARM64%20%7C%20Windows%20x64%2FARM64%20%7C%20macOS-4c8bf5)](https://github.com/Milorgs/DeskCode/releases)

[简体中文](README.md) | English

<p align="center">
  <img src="docs/assets/deskcode-terminal.svg" width="880" alt="DeskCode parallel Git worktree subagents & live LSP diagnostics demo" />
</p>
<p align="center">
  <img src="docs/assets/deskcode-gui.svg" width="880" alt="DeskCode native cross-platform desktop GUI accessibility automation demo" />
</p>

---

## Highlights

- **Single Native C++20 Binary**: Zero Node.js or Python runtime dependencies, instant cold start, and minimal memory footprint—complete with a pinned terminal composer, live streaming Markdown rendering, colored diff approval panels, mouse-wheel transcript scrolling, and a draggable scrollbar.
- **Verified, Reversible Code Editing**: Every write verifies `SHA256` to prevent stale overwrites. Supports single-match `edit_file` and atomic multi-hunk `multi_edit` (`1..20` hunks on files up to `4 MB`) with automatic `CRLF` preservation, `.bak` backups, and one-command `/undo` per turn (retaining unresolved entries for retry on partial failure).
- **Plan Mode, Checklists & Multi-Step Rewind**: Press `Shift+Tab` or type `/plan` and the model may only use read-only tools, then proposes an implementation through `submit_plan`; nothing changes until you approve with `/go`. Multi-step work is tracked live through the model's `todo_write` checklist, and `/rewind` returns to before any earlier request—restoring the conversation and the file changes made since—with the original request back in the composer to edit and resend.
- **Background Commands & Transparent Cost**: `run_shell` can start dev servers or long test runs with `background`, inspected and stopped through `shell_output` / `shell_kill`; press `Ctrl+B` to move a slow foreground command to the background while the turn continues. Tokens are split into input / cache hits / output, and after `/price` the status bar and `/cost` show an estimated cost (OpenRouter's reported cost is used automatically).
- **Three-Platform Kernel Sandbox, Network Isolation & Smart Approvals**: Enforces **Landlock** on Linux, **Apple Seatbelt (`sandbox-exec`)** on macOS, and **Job Object UI/process isolation + Restricted Tokens** on Windows by default; add `--no-net` (or `/sandbox net`) to block child network sockets at the kernel level (Seccomp-BPF on Linux, Seatbelt on macOS). Offers `ask` / `edit` / `full` approval modes and live latest-line command progress.
- **Cross-Platform Desktop Automation + Vision**: Bridges terminal and GUI workflows via Linux `AT-SPI`, Windows `UI Automation`, and macOS `System Events` (up to 500 controls with `filter` support), paired with multimodal `desktop_screenshot` inspection, `/paste` clipboard image attachment, and inline `@image` mentions.
- **Native Dual-Protocol Streaming & Prompt Caching**: Speaks both OpenAI Chat Completions and the native Anthropic Messages API (including native `reasoning_effort` / adaptive `thinking` controls and automatic Anthropic `cache_control` prompt-caching breakpoints), with built-in presets for OpenAI, Anthropic, DeepSeek, Kimi, Qwen, Zhipu GLM, SiliconFlow, OpenRouter, and local Ollama.
- **Compiler-Grade LSP Diagnostics & Navigation**: Automatically discovers installed language servers (`clangd`, `pyright-langserver` / `pylsp`, `typescript-language-server`, `gopls`, `rust-analyzer`, or custom `lsp.json`), attaches compiler errors and warnings after every `edit_file` / `multi_edit` / `write_file`, and exposes `diagnostics`, `definition`, `references`, `hover`, and `symbols` via the `lsp` tool.
- **Writable Parallel Subagents & Git Worktree Isolation**: `delegate_task` and `subagent_output` launch up to 4 concurrent subagents (supporting `.deskcode/agents/*.md`, `.claude/agents/*.md`, and plugin `agents/*.md` custom roles) in isolated `git worktree` branches (`desk/<agent>-<id>`), editing code and running tests in parallel before merging cleanly.
- **Native VS Code Extension, ACP IDEs, Skills/Plugins, GitHub Action & Python SDK**: Ships a native [VS Code extension](editors/vscode/README.md) (`.vsix`) alongside built-in `desk acp` for **Zed** and **JetBrains IDEs**; supports `SKILL.md` skills and `desk plugin` (compatible with Claude Code plugin bundles and `PreToolUse` / `PostToolUse` hooks); includes an official [GitHub Action](action/README.md) (`Milorgs/DeskCode/action@main`) and zero-dependency [Python SDK](sdk/python/README.md) (`deskcode`).
- **`desk serve` Shared-Session Daemon, Complete MCP (with OAuth) & Native Notifications**: `desk serve` hosts a multi-client HTTP + SSE daemon with a built-in responsive **Web Console**, allowing terminals (`desk attach`), browsers, and Python SDK `DeskServeClient` to share live sessions and approve actions remotely; supports native desktop notifications (`--notify` / `/notify`) and full `stdio` + `Streamable HTTP` **MCP** (`tools`, `resources`, `prompts`, and `desk mcp auth` **OAuth 2.0 + S256 PKCE** with automatic token refresh).

---

## Quick Install

Latest version: **1.18.1** (five pre-built platform archives + VS Code `.vsix`: [GitHub Releases](https://github.com/Milorgs/DeskCode/releases))

### Linux (`x86_64` & `arm64`)

```sh
curl -fsSL https://raw.githubusercontent.com/Milorgs/DeskCode/main/install-linux.sh | sh
```

Automatically detects `x86_64` / `aarch64`, downloads the latest Linux release to `~/.local/bin/desk`, and adds `~/.local/bin` to your user `PATH` when needed. Open a new terminal and run `desk`.

### Windows 10/11 (PowerShell `x64` & `ARM64`)

```powershell
irm https://raw.githubusercontent.com/Milorgs/DeskCode/main/install-windows.ps1 | iex
```

Automatically detects `x64` / `ARM64`, downloads the latest Windows package to your user profile directory, and adds it to your user `PATH`. Open a new PowerShell window and run `desk`.

### macOS (Apple Silicon & Intel Universal)

```sh
curl -fsSL https://raw.githubusercontent.com/Milorgs/DeskCode/main/install-macos.sh | sh
```

Downloads the Universal 2 (`arm64` + `x86_64`) self-contained release binary (statically linked—no Homebrew runtime required) to `~/.local/bin/desk` and adds it to your user `PATH` when needed. Open a new terminal and run `desk`.

### Package Managers (Homebrew / Scoop / WinGet / AUR)

```sh
# macOS / Linux (Homebrew)
brew install https://github.com/Milorgs/DeskCode/releases/latest/download/deskcode.rb

# Windows (Scoop)
scoop install https://github.com/Milorgs/DeskCode/releases/latest/download/deskcode.json

# Arch Linux (AUR PKGBUILD)
cd packaging/aur && makepkg -si
```
See [`packaging/README.md`](packaging/README.md) for WinGet and custom tap/bucket maintenance details.

---

## Quick Start

1. Run `desk` to open the main menu (on first launch, **Add model** is automatically preselected).
2. Choose a provider preset (OpenAI, Anthropic, DeepSeek, Kimi, Qwen, Zhipu GLM, SiliconFlow, OpenRouter, or local Ollama).
3. Enter your API key (the base URL is prefilled) and pick a model from the live provider list.
4. Select **`[1] Start chat`**, or launch directly from the CLI:

```sh
desk                                      # Open the interactive menu & multi-chat manager
desk chat                                 # Start a new interactive chat in this workspace
desk -c                                   # Resume the latest chat in this workspace (--continue)
desk "Explain the build and run tests"    # Run a single request and exit (same as -p)
desk doctor                               # Inspect workspace, model, sandbox & desktop status
```

> **Local Credential Isolation**: API keys are stored only in your local user config directory (`~/.config/desk-agent` with `0600` permissions on Linux/macOS, or `%LOCALAPPDATA%\desk-agent` with user-only ACLs on Windows). Environment variables containing `KEY`, `TOKEN`, `SECRET`, or `PASSWORD` are automatically scrubbed from child shell commands and local `stdio` MCP processes.

---

## Built-in Tools (23 + MCP)

| Category | Tool | Description |
|---|---|---|
| **Exploration & Subagents** | `list_files` | List workspace entries with directory-first sorting and byte sizes (`max_depth` `1..4`) |
| | `read_file` | Read full UTF-8 files (`<= 256 KB`) or `start_line..end_line` slices up to `16 MB` (includes full-file `SHA256` up to `4 MB`) |
| | `search_text` | Recursive literal or ECMAScript regex search with `file_pattern` globs (`*.{cpp,hpp}`) and `.gitignore` filtering |
| | `code_outline` | Extract classes, structs, interfaces, enums, functions, and methods with 1-based line numbers |
| | `lsp` | Query the project's language server for `diagnostics`, `definition`, `references`, `hover`, and `symbols` (enabled on demand) |
| | `delegate_research` | Spawn an isolated read-only subagent to investigate multi-file questions without bloating main context |
| | `delegate_task` / `subagent_output` | Spawn writable or custom subagents (`.deskcode/agents/*.md`) with `worktree: true` Git branch isolation and up to 4 concurrent `background: true` workers |
| **Verified Editing** | `edit_file` | Replace a unique exact text match in files up to `4 MB` under `SHA256` verification, attaching fresh LSP diagnostics |
| | `multi_edit` | Atomically apply `1..20` `{old_text, new_text}` hunks to a file under one `SHA256` check, diff preview, and backup |
| | `write_file` | Create or replace a UTF-8 file with automatic `.bak` backup, read-back verification, and LSP diagnostics |
| **Shell & Web** | `run_shell` | Execute noninteractive commands (`30s` default, up to `300s`) with live output progress and Linux Landlock write sandboxing; `background: true` starts long-running commands in the background |
| | `shell_output` / `shell_kill` | Read new output and exit status of a background command / stop it and its children (up to 8, cleaned up when the conversation ends) |
| | `web_search` | Search the web for titles, URLs, and snippets (customizable via `DESK_SEARCH_URL`) |
| | `web_fetch` | Fetch public HTTPS pages (or `localhost` HTTP) into clean text with SSRF private-IP blocking and DNS pinning |
| **Desktop & Vision** | `desktop_snapshot` | Inspect open desktop apps or up to 500 accessibility controls for an app (with optional `filter`) |
| | `desktop_action` | Trigger an accessibility action advertised by a control in the latest snapshot |
| | `desktop_set_text` | Replace text in an editable desktop control |
| | `desktop_screenshot` | Capture the active window or desktop as PNG and attach it as a multimodal vision block |
| **Skills & MCP Ecosystem** | `skill` | Load on-demand `SKILL.md` domain skills and bundled reference files from workspace, user, or plugins (omitted when none exist) |
| | `mcp_<server>_<tool>` / `mcp_read_resource` / `mcp_get_prompt` | Dynamically attach tools, read-only `resources`, and `prompts` from local `stdio` or remote `Streamable HTTP` MCP servers (with OAuth 2.0 auto-refresh) |
| **Planning & Progress** | `todo_write` | The model keeps a checklist (pending / in progress / completed) for multi-step work, shown live in the terminal |
| | `submit_plan` | Plan mode only: submit the full implementation plan for approval; no file changes before approval |

---

## Interactive Commands & Shortcuts

### Slash Commands & Mentions

| Command / Syntax | Action |
|---|---|
| `@query` + `Tab` | Fuzzy-complete workspace file paths (respects `.gitignore`) |
| `@file` / `@file:start-end` | Inline file contents, line ranges, or workspace images (`PNG` / `JPEG` / `WebP` / `GIF`) |
| `/` | Open the interactive chat command palette |
| `/undo` | Revert all file edits and newly created files from the last turn |
| `/plan [request]` (or `Shift+Tab`) | Toggle plan mode: investigate and propose without changing files; with a request, send it in plan mode |
| `/go [notes]` | Approve the current plan, leave plan mode, and start implementing |
| `/rewind` | Rewind to before an earlier request: conversation and file changes, or the conversation only |
| `/cost` · `/price IN OUT [CACHE] [CURRENCY]` | Show tokens split by input / cache hits / output with estimated cost · Set per-million-token prices for the current model (`/price off` clears) |
| `/bg` (or `Ctrl+B`) | Move the running command to the background without stopping the turn |
| `/compact` | Summarize long conversation history into a dense continuation brief |
| `/paste [prompt]` | Attach an image directly from the system clipboard (or press `Ctrl+V`) |
| `/sandbox [net]` · `/notify` | Toggle the kernel filesystem sandbox or child network isolation (`net`) · Toggle native desktop notifications |
| `/commands` · `/skills` · `/plugins` · `/agents` | List custom slash commands · List discovered `SKILL.md` skills · List installed plugins · List built-in & custom subagents |
| `/approval` (or `/perm`) | Switch approval mode (`Ask for approval` / `Auto-approve edits` / `Full access`) |
| `/model` · `/reasoning` | Switch active model profile · Adjust native reasoning effort |
| `/mcp` · `/status` · `/clear` · `/new` · `/help` | Manage MCP servers · Show status & token usage · Clear context · New chat · Shortcut help |

### Keyboard & Mouse Navigation

- **`F2` / `F3`**: Switch model profile / adjust reasoning effort.
- **`Shift+Tab`**: Toggle plan mode; **`Ctrl+B`**: Move the running command to the background (Linux / macOS).
- **`Alt+Enter`**: Insert a newline in the composer; pasted multiline snippets fold automatically without premature submission.
- **`Ctrl+P` / `Ctrl+N` (or `Alt+Up` / `Alt+Down`)**: Recall earlier prompts sent in the current conversation.
- **Mouse wheel / Scrollbar drag / `PgUp` `PgDn` / `Ctrl+Up/Down`**: Scroll conversation history and approval diffs; hold **`Shift` + drag** to select terminal text.
- **`Ctrl+C`**: Cancel the active turn or shell command; **`Ctrl+D`**: Return to the conversation picker while background tasks continue running.

---

## Project Customization & Extensions

### 1. Project Instructions (`DESKCODE.md`)
Place a `DESKCODE.md` (or `deskcode.md`, also compatible with `AGENTS.md` and `CLAUDE.md`) in your workspace root to automatically provide build commands, architecture notes, and coding conventions to the model (disable anytime with `DESK_NO_PROJECT_GUIDE=1`).

### 2. Custom Slash Commands (`.deskcode/commands/*.md`)
Add Markdown prompt templates under `.deskcode/commands/`, `.desk-agent/commands/`, or `~/.config/desk-agent/commands/` and invoke them with `/<name> [arguments]`:

```markdown
<!-- .deskcode/commands/review.md -->
Review $ARGUMENTS for concurrency hazards, error handling, edge cases, and memory safety.
```

### 3. Lifecycle Hooks (`hooks.json`)
Configure `pre_shell` and `post_edit` hooks in `~/.config/desk-agent/hooks.json` or workspace `.desk-agent/hooks.json` (workspace hooks prompt for approval before first execution):

```json
{
  "pre_shell": "if echo \"$DESK_HOOK_CMD\" | grep -q 'git push --force'; then echo 'Force push blocked by policy'; exit 1; fi",
  "post_edit": [
    {
      "pattern": "*.{cpp,hpp}",
      "command": "make -j4 build/desk"
    }
  ]
}
```
- A non-zero `pre_shell` exit blocks the command and returns the hook output to the model.
- `post_edit` runs after file writes and appends `hook_exit_code` and `hook_output` (compiler/linter feedback) directly to the tool result so the model can self-correct.

### 4. MCP Servers (`stdio` & `Streamable HTTP`)
Import from a file, paste multiline JSON, or enter a remote URL via `/mcp`:

```json
{
  "mcpServers": {
    "filesystem": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-filesystem", "."]
    },
    "remote-docs": {
      "url": "https://example.com/mcp",
      "headers": {
        "Authorization": "Bearer YOUR_TOKEN"
      }
    }
  }
}
```

### 5. Skills & Plugin Ecosystem (Claude Code-Compatible)
- **Skills (`SKILL.md`)**: Place `<name>/SKILL.md` (with YAML frontmatter `name` / `description` and optional reference files) under `.deskcode/skills/`, `.claude/skills/`, or `~/.config/desk-agent/skills/`. The model loads instructions on demand via `skill`, or invoke directly with `/<name> [request]`.
- **Plugins (`desk plugin`)**: Install bundles containing `commands/`, `skills/`, `agents/`, `hooks/hooks.json` (supporting both `pre_shell`/`post_edit` and Claude `PreToolUse`/`PostToolUse` matchers), and `.mcp.json`:

```sh
desk plugin install owner/repo             # Install from GitHub owner/repo, Git URL, or local directory
desk plugin install owner/marketplace pkg  # Install a named plugin from a marketplace
desk plugin list                           # Inspect installed plugins and component counts
desk plugin disable pkg && desk plugin enable pkg
desk plugin validate ./my-plugin           # Validate manifest and bundle layout
```

### 6. IDE Integration & `desk serve` Shared-Session Daemon
- **VS Code Extension ([`editors/vscode/`](editors/vscode/README.md))**: Download `deskcode-vscode-1.18.1.vsix` from [GitHub Releases](https://github.com/Milorgs/DeskCode/releases) and run `code --install-extension deskcode-vscode-1.18.1.vsix` for a native sidebar chat with 4 modes, native side-by-side diffs, inline permission approvals, and editor context actions (*Ask DeskCode About Selection* / *Fix Diagnostics in File*).
- **Shared-Session Daemon (`desk serve` + `desk attach`)**: Run `desk serve --port 7890 --notify` to open the built-in Web Console (`http://127.0.0.1:7890`) and attach from any terminal (`desk attach -p "..."`), browser, or Python `DeskServeClient` with live remote approvals.
- **Zed / JetBrains ACP**: Run `desk acp` to expose DeskCode over ACP v1 (`stdio` newline-delimited JSON-RPC 2.0) in **Zed**, **JetBrains IDEs**, and other ACP-capable editors. Example Zed `settings.json`:

```json
{
  "agent_servers": {
    "DeskCode": {
      "command": "desk",
      "args": ["acp"]
    }
  }
}
```

### 7. GitHub Action & Python SDK
- **GitHub Action ([`action/`](action/README.md))**: Add `uses: Milorgs/DeskCode/action@main` to respond to `@desk <request>` on issues and PRs, automatically verifying collaborator write access, redacting secrets, and opening pull requests.
- **Python SDK ([`sdk/python/`](sdk/python/README.md))**: `pip install ./sdk/python` installs the zero-dependency `deskcode` package with one-shot `deskcode.run(...)` / `arun(...)` and streaming ACP sessions via `DeskAgent` / `AsyncDeskAgent`.

---

## Headless Mode & Scripting

When `stdout` is piped or redirected, DeskCode writes only the final reply to `stdout` while routing progress to `stderr`. Pass `--json` for a single machine-readable JSON object:

```sh
# Write only the final assistant reply to a file
desk -p "Audit this repository and list the top 3 risks" > report.md

# Pipe a diff over stdin and auto-approve workspace edits + read-only commands
git diff | desk -p - --approval edit

# Emit structured JSON (status, reply, tokens, usage split into input / cache_read / output, elapsed_seconds, tools trace)
desk --json -p "Run the test suite and fix any failing test" --approval edit --max-steps 25

# Plan mode: investigate and print the implementation plan without changing files
desk --plan -p "Add rate limiting to the login module and propose the changes" > plan.md

# Machine-readable environment health check
desk doctor --json
```

- **Approval flags**: `--approval ask` (default) | `--approval edit` (auto-approves workspace file edits and safe read-only commands) | `--approval full` (alias `--yes`; combine with `--no-sandbox` when needed).
- **Exit codes**: `0` completed · `1` not completed (including `approval_required` when no interactive TTY is available) · `2` usage error · `130` interrupted.

---

## Agent Benchmarks

[`bench/`](bench/README.md) provides reproducible scoring: an offline local task suite (`python3 bench/run_local.py`, model chosen through `DESK_*` variables, run with isolated configuration directories, reporting pass rate, time and tokens), a Terminal-Bench 2.0 / Harbor adapter (`x86_64` & `arm64`), and a multi-model leaderboard generator (`python3 bench/render_leaderboard.py`).

---

## Building from Source & Testing

### Linux / macOS
Dependencies: C++20 compiler (GCC 11+ / Clang 14+), `make`, `pkg-config`, `libcurl`, `json-c`, `openssl`, `readline`, plus `libatspi2.0-dev` on Linux for desktop accessibility tools.

```sh
make -j"$(nproc)"          # Build build/desk
make test                  # Run the unit & integration test suite (~5310+ assertions)
make test-mcp-http         # Run OpenAI / Anthropic + MCP Streamable HTTP end-to-end tests
make sanitize              # Run with AddressSanitizer + UndefinedBehaviorSanitizer
./install.sh               # Install to ~/.local/bin/desk
```

### Windows (MSVC + CMake + vcpkg)

```powershell
cmake -S . -B build/windows -A x64 "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static -DBUILD_TESTING=ON
cmake --build build/windows --config Release --parallel
ctest --test-dir build/windows -C Release --output-on-failure
```

---

## Links

- **Pre-built Binaries**: [GitHub Releases](https://github.com/Milorgs/DeskCode/releases)
- **Release Notes**: [docs/releases/](docs/releases/)
- **Engineering History**: [docs/development.md](docs/development.md)
