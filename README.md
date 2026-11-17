# DeskCode

## 我做了什么 / What I built

我做了 `DeskCode`，一个用终端操作的 CLI AI 助手。我用它编辑本地文件、执行终端任务、操作桌面应用，也可以同时运行多个对话。

I built `DeskCode`, a terminal AI assistant. I use it to edit local files, run terminal tasks, control desktop applications, and keep multiple conversations running at the same time.

支持 Linux x86_64 和 Windows 10/11 x64。模型接口支持 OpenAI 兼容接口和 Anthropic 原生接口，也支持导入 MCP 工具。

It supports Linux x86_64 and Windows 10/11 x64, OpenAI-compatible APIs, the native Anthropic API, and imported MCP tools.

## 一行安装 / One-line install

### Linux

```sh
curl -fsSL https://raw.githubusercontent.com/Milorgs/desk-agent/main/install-linux.sh | sh
```

脚本会下载最新 Linux 版本，安装到 `~/.local/bin/desk`，并尝试加入当前用户的 PATH。安装后重新打开终端，输入 `desk`。

This downloads the latest Linux release, installs `desk` to `~/.local/bin`, and adds it to your PATH when possible. Open a new terminal and run `desk`.

### Windows PowerShell

```powershell
irm https://raw.githubusercontent.com/Milorgs/desk-agent/main/install-windows.ps1 | iex
```

脚本会下载最新 Windows 版本，安装到当前用户目录，并加入用户 PATH。安装后重新打开 PowerShell，输入 `desk`。

This downloads the latest Windows release, installs it for the current user, and adds it to the user PATH. Open a new PowerShell window and run `desk`.

手动下载： [GitHub Releases](https://github.com/Milorgs/desk-agent/releases)。

Manual downloads: [GitHub Releases](https://github.com/Milorgs/desk-agent/releases).

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

- 用自然语言读取、搜索、编辑工作目录内的文件；改动前在审批面板里看到带颜色的 diff。
- 执行终端命令，并在需要时请求批准。
- 通过 Linux AT-SPI 或 Windows UI Automation 操作支持标准控件的桌面应用。
- 使用多个模型配置，选择后会成为下次启动的默认模型。
- 在对话列表中切换对话；Linux 下按 `Ctrl+D` 返回列表，任务会继续在后台运行。`desk -c` 直接继续最近的对话。
- 导入本地 stdio 或远程 Streamable HTTP MCP 服务。
- 回复按 Markdown 排版，代码块、列表和标题在终端里清晰可读。

- Read, search, and edit files inside the workspace with natural language, and review a coloured diff before each change.
- Run terminal commands and ask for approval when needed.
- Control desktop applications through Linux AT-SPI or Windows UI Automation.
- Keep multiple model profiles; the selected profile becomes the default on the next launch.
- Switch conversations; on Linux, `Ctrl+D` returns to the list while the task keeps running. `desk -c` resumes the latest one.
- Import local stdio or remote Streamable HTTP MCP servers.
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

对话中按 `/` 打开命令面板（选择模型、推理模式、批准模式、新建对话、清空上下文、查看状态、快捷键帮助等），`F2` 切换模型，`F3` 调整推理强度。鼠标放进对话窗口直接用滚轮上下滚动审查对话（审批 diff 时同样可滚轮翻阅），也可以按住右侧滚动条拖动定位；按住 `Shift` 拖动鼠标选择文字，也支持 `↑/↓`、`PgUp/PgDn` 翻页、`Ctrl+↑/↓` 逐行滚动，二级菜单按 `Esc` 返回。Linux 下：

- `Ctrl+D`：返回选择对话，任务继续运行。
- `Ctrl+C`：停止当前任务。
- `0`：普通需求内容，不是退出键。

In a chat, press `/` for the command palette (model, reasoning, approval, new chat, clear context, status, keyboard shortcuts, and more), `F2` to switch models, or `F3` to adjust reasoning. Hover the mouse inside the conversation and use the scroll wheel to review messages and approval diffs or drag the right-hand scrollbar to navigate; hold `Shift` while dragging to select text, or use `Up/Down`, `PgUp/PgDn` by page, and `Ctrl+Up/Down` by line; `Esc` leaves submenus. On Linux:

- `Ctrl+D`: return to the conversation list while the task continues.
- `Ctrl+C`: stop the current task.
- `0`: normal request text, not an exit key.

## 安装包和源码 / Packages and source

最新版本：**1.11.4**。Linux 包基于 Ubuntu 22.04 构建，需要 glibc 2.35+ 及系统运行库；Windows 包面向 Windows 10/11 x64。

Latest version: **1.11.4**. The Linux package is built on Ubuntu 22.04 and requires glibc 2.35+ plus system runtime libraries. The Windows package targets Windows 10/11 x64.

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

- [GitHub repository](https://github.com/Milorgs/desk-agent)
- [Releases](https://github.com/Milorgs/desk-agent/releases)
- [开发记录 / Development notes](docs/development.md)

我会继续把可运行版本和 Windows/Linux 安装包放到 GitHub。

I will keep publishing working versions and Windows/Linux packages on GitHub.
