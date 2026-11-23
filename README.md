# DeskCode

## 我做了什么 / What I built

我做了 `DeskCode`，一个用终端操作的 CLI AI 助手。我用它编辑本地文件、执行终端任务、操作桌面应用，也可以同时运行多个对话。

I built `DeskCode`, a terminal AI assistant. I use it to edit local files, run terminal tasks, control desktop applications, and keep multiple conversations running at the same time.

支持 Linux x86_64、Windows 10/11 x64 与 macOS 源码构建。模型接口支持 OpenAI 兼容接口和 Anthropic 原生接口（含自动 `cache_control` 提示词缓存断点），也支持导入 MCP 工具、项目约定 `DESKCODE.md`、生命周期钩子（`hooks.json`）与自定义斜杠指令（`.deskcode/commands/*.md`）。

It supports Linux x86_64, Windows 10/11 x64, and macOS source builds, OpenAI-compatible APIs, the native Anthropic API (with automatic `cache_control` prompt caching), imported MCP tools, `DESKCODE.md` project instructions, lifecycle hooks (`hooks.json`), and custom slash commands (`.deskcode/commands/*.md`).

## 一行安装 / One-line install

### Linux

```sh
curl -fsSL https://raw.githubusercontent.com/Milorgs/DeskCode/main/install-linux.sh | sh
```

脚本会下载最新 Linux 版本，安装到 `~/.local/bin/desk`，并尝试加入当前用户的 PATH。安装后重新打开终端，输入 `desk`。

This downloads the latest Linux release, installs `desk` to `~/.local/bin`, and adds it to your PATH when possible. Open a new terminal and run `desk`.

### Windows PowerShell

```powershell
irm https://raw.githubusercontent.com/Milorgs/DeskCode/main/install-windows.ps1 | iex
```

脚本会下载最新 Windows 版本，安装到当前用户目录，并加入用户 PATH。安装后重新打开 PowerShell，输入 `desk`。

This downloads the latest Windows release, installs it for the current user, and adds it to the user PATH. Open a new PowerShell window and run `desk`.

手动下载： [GitHub Releases](https://github.com/Milorgs/DeskCode/releases)。

Manual downloads: [GitHub Releases](https://github.com/Milorgs/DeskCode/releases).

## 第一次使用 / First run

1. 输入 `desk`。没有模型时，主菜单会提示并默认选中「添加模型」。
2. 选择服务商：OpenAI、Anthropic、DeepSeek、Kimi、通义千问、智谱 GLM、硅基流动、OpenRouter 或 Ollama 本地模型。
3. 填入 API Key（地址已预填），从在线模型列表中选一个模型。
4. 选择 `[1] 开始对话`，直接描述要做的事。

1. Run `desk`. With no model configured, the main menu says so and preselects Add model.
2. Pick a provider: OpenAI, Anthropic, DeepSeek, Kimi, Qwen, Zhipu GLM, SiliconFlow, OpenRouter, or local Ollama.
3. Enter the API key (the URL is prefilled) and choose a model from the provider's live list.
4. Choose `[1] Start chat` and describe what you want done.

API Key 只保存在我运行 `desk` 的本机用户配置目录，不会写入仓库或安装包。随时可以运行 `desk doctor` 检查环境。

API keys stay in the local user configuration directory where I run `desk`. They are not included in the repository or release packages. Run `desk doctor` at any time to check the setup.

## 我能做什么 / What I can do

- 用自然语言读取、搜索、编辑工作目录内的文件（`edit_file` 与单次多处原子替换的 `multi_edit` 支持最大 4 MB 源码文件）；改动前在审批面板里看到带颜色的 diff，随时输入 `/undo` 一键撤回上一轮修改与新建文件。
- 用 `code_outline` 提取源码符号与行号大纲，用支持正则、`file_pattern` 通配符与 `.gitignore` 过滤的 `search_text` 精准搜索，用 `delegate_research` 发起只读子任务调研，用 `web_search` 与 `web_fetch` 联网检索并读取网页，自动加载项目根目录的 `DESKCODE.md` / `deskcode.md`（兼容 `AGENTS.md`）、生命周期钩子（`post_edit` / `pre_shell`）与自定义斜杠指令。
- 输入 `@关键词` 按 `Tab` 模糊补全文件，发送 `@文件` 或 `@文件:起-止` 直接附带文件/图片内容；用 `/paste` 附带剪贴板图片，多行粘贴自动折叠不误提交；用 `/compact` 主动将长对话提炼为高密度续接摘要。
- 执行终端命令并实时显示最新输出行进度；Linux 下默认启用 **Landlock 内核级文件系统沙箱**禁止命令修改工作目录外的宿主文件，「帮我批准」模式下自动放行安全的只读检查命令，命令审批时可选「本对话允许同类」。
- 通过 Linux AT-SPI、Windows UI Automation 或 macOS System Events 操作桌面应用（支持最多 500 个节点与 `filter` 过滤），并支持 `desktop_screenshot` 桌面多模态视觉截图。
- 使用多个模型配置，选择后会成为下次启动的默认模型。
- 在对话列表中切换对话；按 `Ctrl+D` 返回列表，任务会继续在后台运行。`desk -c` 直接继续最近的对话。
- 在 Linux、Windows 和 macOS 上均可导入本地 stdio 或远程 Streamable HTTP MCP 服务。
- 回复按 Markdown 排版，代码块、列表和标题在终端里清晰可读。

- Read, search, and edit files inside the workspace (`edit_file` and multi-hunk atomic `multi_edit` support files up to 4 MB), review a coloured diff before each change, and run `/undo` to revert the last turn's file edits and newly created files in one step.
- Map source symbols and line numbers with `code_outline`, search with regex, `file_pattern` globs, and `.gitignore` awareness in `search_text`, delegate multi-file investigation with read-only `delegate_research`, search and fetch documentation pages with `web_search` + `web_fetch`, and load workspace `DESKCODE.md` / `deskcode.md` (compatible with `AGENTS.md`), `post_edit` / `pre_shell` hooks, and custom slash commands automatically.
- Type `@query` + `Tab` to fuzzy-complete workspace files, attach `@file` or `@file:start-end` slices and images inline, run `/paste` to attach clipboard images, paste multiline snippets safely without premature submission, and run `/compact` to condense long conversations into a dense continuation summary.
- Run terminal commands with live latest-line progress; on Linux, the **Landlock kernel sandbox** restricts shell writes to the workspace and `/tmp` by default, auto-edit mode lets verified read-only inspection commands run without friction, and shell approvals include an `Allow similar` option for the current chat.
- Control desktop applications through Linux AT-SPI, Windows UI Automation, or macOS System Events (up to 500 controls with `filter` support) and inspect UI state visually with `desktop_screenshot`.
- Keep multiple model profiles; the selected profile becomes the default on the next launch.
- Switch conversations; `Ctrl+D` returns to the list while tasks keep running in the background. `desk -c` resumes the latest one.
- Import local stdio or remote Streamable HTTP MCP servers on Linux, Windows, and macOS.
- Replies render as Markdown, so code blocks, lists and headings stay readable in the terminal.

MCP 菜单支持从文件导入、直接粘贴多行 JSON，以及查看或保存配置示例。也可以直接填写远程 MCP 地址。

The MCP menu accepts a configuration file or pasted multiline JSON and can show or save an example. You can also enter a remote MCP URL directly.

对话会自动保存在本机，按工作目录分别显示。重开后可以继续、重命名或删除。Linux 默认位置是 `~/.config/desk-agent/history/`，Windows 是 `%LOCALAPPDATA%\desk-agent\history\`。

Chats are saved locally and listed by workspace. Reopen them to continue, rename, or delete. The default locations are `~/.config/desk-agent/history/` on Linux and `%LOCALAPPDATA%\desk-agent\history\` on Windows.

## 脚本与 AI 调用 / Scripts and agents

```sh
desk -p "检查当前项目并告诉我最重要的问题" > report.md   # 标准输出只有最终回复
git diff | desk -p - --approval edit                      # 从标准输入读取需求
desk --json -p "运行测试并修复失败用例" --approval edit    # 一个 JSON 结果
desk doctor --json                                        # 机器可读的环境检查
```

`--json` 输出 `status`（`completed`、`failed`、`approval_required`、`denied`、`interrupted`、`step_limit`、`not_configured`）、`reply`、`error`、`tokens`、`elapsed_seconds` 和 `tools`（每次工具调用的名称、目标与结果）。没有终端时如需批准，会返回 `approval_required`，用 `--approval edit` 或 `--approval full` 重新运行。退出码：0 完成、1 未完成、2 参数错误、130 已中断。

`--json` prints `status` (`completed`, `failed`, `approval_required`, `denied`, `interrupted`, `step_limit`, `not_configured`), `reply`, `error`, `tokens`, `elapsed_seconds`, and `tools` (name, target and result of each tool call). Without a terminal, a needed approval returns `approval_required`; rerun with `--approval edit` or `--approval full`. Exit codes: 0 done, 1 not completed, 2 usage error, 130 interrupted.

## 菜单和快捷键 / Menus and keys

```text
[1] 开始对话 / Start chat
[2] 选择模型 / Select model
[3] 添加模型 / Add model
[4] MCP 服务 / MCP servers
[5] 设置 / Settings
[0] 退出 / Exit
```

对话中按 `/` 打开命令面板（选择模型、推理模式、批准模式、新建对话、清空上下文、查看状态、快捷键帮助等），`F2` 切换模型，`F3` 调整推理强度。鼠标放进对话窗口直接用滚轮上下滚动审查对话（审批 diff 时同样可滚轮翻阅），也可以按住右侧滚动条拖动定位；按住 `Shift` 拖动鼠标选择文字，也支持 `↑/↓`、`PgUp/PgDn` 翻页、`Ctrl+↑/↓` 逐行滚动；`Ctrl+P/Ctrl+N`（或 `Alt+↑/↓`）调出本对话之前发送的需求，底部状态栏显示估算的上下文占用比例。二级菜单按 `Esc` 返回。Linux 下：

- `Ctrl+D`：返回选择对话，任务继续运行。
- `Ctrl+C`：停止当前任务。
- `0`：普通需求内容，不是退出键。

In a chat, press `/` for the command palette (model, reasoning, approval, new chat, clear context, status, keyboard shortcuts, and more), `F2` to switch models, or `F3` to adjust reasoning. Hover the mouse inside the conversation and use the scroll wheel to review messages and approval diffs or drag the right-hand scrollbar to navigate; hold `Shift` while dragging to select text, or use `Up/Down`, `PgUp/PgDn` by page, and `Ctrl+Up/Down` by line. `Ctrl+P/Ctrl+N` (or `Alt+Up/Down`) recall earlier requests of the conversation, and the status line shows an estimate of how much of the context budget is in use. `Esc` leaves submenus. On Linux:

- `Ctrl+D`: return to the conversation list while the task continues.
- `Ctrl+C`: stop the current task.
- `0`: normal request text, not an exit key.

## 安装包和源码 / Packages and source

最新版本：**1.14.1**。Linux 包基于 Ubuntu 22.04 构建，需要 glibc 2.35+ 及系统运行库；Windows 包面向 Windows 10/11 x64；macOS 支持直接从源码构建。

Latest version: **1.14.1**. The Linux package is built on Ubuntu 22.04 and requires glibc 2.35+ plus system runtime libraries; the Windows package targets Windows 10/11 x64; macOS builds directly from source.

也可以直接进入对话或执行一次请求：

```sh
desk chat
desk -p "检查当前项目并告诉我最重要的问题"
```

You can also start a chat directly or run one request:

```sh
desk chat
desk -p "Review this project and tell me the most important issue"
```

我只在需要开发或修改源码时从源码构建：

I build from source only when I need to develop or modify the code:

```sh
make
./build/desk
```

源码构建需要 C++20、Make、pkg-config、libcurl、json-c、OpenSSL、Readline；Linux 桌面功能还需要 AT-SPI 开发库。

A source build needs C++20, Make, pkg-config, libcurl, json-c, OpenSSL, and Readline. Linux desktop support also needs AT-SPI development files.

## 项目链接 / Links

- [GitHub repository](https://github.com/Milorgs/DeskCode)
- [Releases](https://github.com/Milorgs/DeskCode/releases)
- [开发记录 / Development notes](docs/development.md)

我会继续把可运行版本和 Windows/Linux 安装包放到 GitHub。

I will keep publishing working versions and Windows/Linux packages on GitHub.
