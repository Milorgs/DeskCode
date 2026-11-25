# DeskCode

**原生 C++20 跨平台终端与桌面 AI 编程助手** — 集成本地代码精确编辑、Linux Landlock 内核沙箱命令执行、多模态桌面无障碍自动化、OpenAI / Anthropic 原生双协议流式推理与 MCP 工具生态。

[![Build and tests](https://github.com/Milorgs/DeskCode/actions/workflows/test.yml/badge.svg)](https://github.com/Milorgs/DeskCode/actions/workflows/test.yml)
[![Latest Release](https://img.shields.io/github/v/release/Milorgs/DeskCode?label=release)](https://github.com/Milorgs/DeskCode/releases)
[![Standard](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](CMakeLists.txt)
[![Platforms](https://img.shields.io/badge/platform-Linux%20x64%2FARM64%20%7C%20Windows%20x64%2FARM64%20%7C%20macOS-4c8bf5)](https://github.com/Milorgs/DeskCode/releases)

简体中文 | [English](README_EN.md)

<p align="center">
  <img src="docs/assets/deskcode-terminal.svg" width="880" alt="DeskCode 并行 Git Worktree 与 LSP 自动诊断演示" />
</p>
<p align="center">
  <img src="docs/assets/deskcode-gui.svg" width="880" alt="DeskCode 跨平台原生桌面 GUI 无障碍操控演示" />
</p>

---

## 核心特性

- **原生 C++20 单二进制交付**：无需 Node.js 或 Python 运行时，毫秒级启动、低内存占用；终端内置底部固定输入栏、实时流式 Markdown 排版、彩色 Diff 审批框、鼠标滚轮翻阅与右侧拖拽滚动条。
- **可校验、可撤回的原子代码编辑**：写入强制核对 `SHA256` 杜绝盲改覆盖；支持单处精确替换 `edit_file` 与单次最多 20 处原子替换 `multi_edit`（最大 `4 MB` 源码文件），自动兼容 `CRLF` 换行，支持 `/undo` 一键回滚上一轮修改与新建文件（部分失败自动保留进度可重试）。
- **计划模式、任务清单与多步回退**：`Shift+Tab` 或 `/plan` 进入计划模式，模型只能使用只读工具调研并通过 `submit_plan` 提交方案，`/go` 批准后才开始改动；多步任务由模型用 `todo_write` 维护实时任务清单；`/rewind` 可回退到任意一条历史需求之前，同时撤回对话与该轮之后的文件改动，并把原需求放回输入框以便修改重发。
- **后台命令与用量费用透明**：`run_shell` 支持 `background` 启动开发服务器或长时间测试，`shell_output` / `shell_kill` 随时查看与停止；命令跑太久时按 `Ctrl+B` 直接转入后台、任务继续。按「输入 / 缓存命中 / 输出」拆分统计词元，`/price` 设置模型单价后状态栏与 `/cost` 实时显示估算费用（OpenRouter 自动读取官方计费）。
- **三平台内核级沙箱、可选禁网与细粒度审批**：Linux 默认启用 **Landlock** 内核写保护，macOS 默认启用 **Apple Seatbelt（`sandbox-exec`）** 写保护，Windows 默认启用 **Job Object UI/进程隔离 + Restricted Token** 特权剥离；叠加 `--no-net`（或 `/sandbox net`）可通过 Linux **Seccomp-BPF** / macOS **Seatbelt** 彻底拦截子进程网络连接。支持 `ask` / `edit` / `full` 三档批准模式与实时命令尾部进度。
- **跨平台桌面无障碍自动化 + 多模态视觉**：原生对接 Linux `AT-SPI`、Windows `UI Automation` 与 macOS `System Events`（支持最多 500 个控件节点与 `filter` 过滤），配合 `desktop_screenshot` 桌面视觉截图、`/paste` 剪贴板贴图与 `@图片` 内联输入，打通终端与 GUI 应用边界。
- **双协议原生推理与提示词缓存**：原生支持 OpenAI Chat Completions 与 Anthropic Messages API（含自适应推理强度 `reasoning_effort` / `thinking` 与 Anthropic 自动 `cache_control` 提示词缓存断点），内置 OpenAI、Anthropic、DeepSeek、Kimi、通义千问、智谱 GLM、硅基流动、OpenRouter 与本地 Ollama 预设。
- **LSP 代码诊断与导航**：自动发现本机语言服务（`clangd`、`pyright-langserver` / `pylsp`、`typescript-language-server`、`gopls`、`rust-analyzer` 或自定义 `lsp.json`），每次 `edit_file` / `multi_edit` / `write_file` 后自动回传编译器错误与警告，并通过 `lsp` 工具支持跳转定义、查找引用、悬停类型与文档符号。
- **可写并行子代理与 Git Worktree 隔离**：通过 `delegate_task` + `subagent_output` 可并发启动最多 4 个独立子代理（支持 `.deskcode/agents/*.md`、`.claude/agents/*.md` 与插件 `agents/*.md` 自定义角色），在各自临时 `git worktree` 分支（`desk/<agent>-<id>`）中并行改代码跑测试，完成后零冲突合并回主分支。
- **IDE 原生扩展（VS Code + ACP）、Skills/插件生态、GitHub Action 与 Python SDK**：提供 [VS Code 原生扩展](editors/vscode/README.md)（`.vsix`）以及内置 `desk acp` 直连 **Zed** 与 **JetBrains IDEs**；支持 `SKILL.md` 技能与 `desk plugin` 插件管理器（兼容 Claude Code 插件包与 `PreToolUse` / `PostToolUse` 钩子）；随附官方 [GitHub Action](action/README.md)（`Milorgs/DeskCode/action@main`）与零依赖 [Python SDK](sdk/python/README.md)（`deskcode`）。
- **`desk serve` 多端共享守护进程、全协议 MCP（含 OAuth）与系统通知**：`desk serve` 启动本地 HTTP + SSE 守护服务与内置响应式 **Web 控制台**，终端 `desk attach`、浏览器与 Python SDK `DeskServeClient` 可同时挂载并远程审批同一个实时会话；后台对话完成或待批准支持原生桌面通知（`--notify` / `/notify`）；完整支持本地 `stdio` 与远程 `Streamable HTTP` **MCP 服务**（含 `tools`、`resources`、`prompts` 与 `desk mcp auth` **OAuth 2.0 + S256 PKCE** 自动续期）。

---

## 快速安装

当前最新版本：**1.18.5**（五平台预编译包与 VS Code `.vsix`：[GitHub Releases](https://github.com/Milorgs/DeskCode/releases)）

### Linux (`x86_64` & `arm64`)

```sh
curl -fsSL https://raw.githubusercontent.com/Milorgs/DeskCode/main/install-linux.sh | sh
```

脚本自动识别 `x86_64` / `aarch64` 下载最新 Linux 发行包并安装至 `~/.local/bin/desk`。重新打开终端后输入 `desk` 即可启动。

### Windows 10/11 (PowerShell `x64` & `ARM64`)

```powershell
irm https://raw.githubusercontent.com/Milorgs/DeskCode/main/install-windows.ps1 | iex
```

脚本自动识别 `x64` / `ARM64` 下载最新 Windows 发行包到当前用户目录并加入用户 `PATH`。重新打开 PowerShell 后输入 `desk` 即可启动。

### macOS (Apple Silicon & Intel Universal)

```sh
curl -fsSL https://raw.githubusercontent.com/Milorgs/DeskCode/main/install-macos.sh | sh
```

脚本自动下载通用（`arm64` + `x86_64` Universal 2）独立发行包（已静态链接依赖，无需安装 Homebrew 运行库）并安装至 `~/.local/bin/desk`。重新打开终端后输入 `desk` 即可启动。

### 包管理器安装（Homebrew / Scoop / WinGet / AUR）

```sh
# macOS / Linux (Homebrew)
brew install https://github.com/Milorgs/DeskCode/releases/latest/download/deskcode.rb

# Windows (Scoop)
scoop install https://github.com/Milorgs/DeskCode/releases/latest/download/deskcode.json

# Arch Linux (AUR PKGBUILD)
cd packaging/aur && makepkg -si
```
更多 WinGet 与自建软件源维护说明见 [`packaging/README.md`](packaging/README.md)。

---

## 30 秒上手

1. 运行 `desk` 打开主菜单（首次未配置模型时会自动高亮引导 **「添加模型」**）。
2. 选择内置服务商预设（DeepSeek、Kimi、通义千问、智谱 GLM、硅基流动、OpenRouter、OpenAI、Anthropic 或本地 Ollama）。
3. 填入 API Key（接口地址已自动预填），直接从拉取的在线模型列表中选择目标模型。
4. 选择 **`[1] 开始对话`**，或使用命令行快捷入口：

```sh
desk                        # 打开交互主菜单与多对话管理器
desk chat                   # 在当前目录直接新建对话
desk -c                     # 继续当前目录最近一次对话（同 --continue）
desk "梳理项目架构并运行测试"  # 单次执行需求后退出（同 -p）
desk doctor                 # 检查工作区权限、模型配置、沙箱与桌面支持状态
```

> **本地密钥与安全隔离**：API Key 仅以私有权限（Linux/macOS `0600`，Windows 当前用户独占 ACL）保存在本机配置目录（`~/.config/desk-agent` 或 `%LOCALAPPDATA%\desk-agent`）。执行 Shell 命令与本地 MCP 子进程时，所有含 `KEY`、`TOKEN`、`SECRET`、`PASSWORD` 的敏感环境变量均会被自动剥离。

---

## 内置工具矩阵（23 项 + MCP）

| 类别 | 工具名 | 功能说明 |
|---|---|---|
| **代码浏览与子代理** | `list_files` | 列出工作区目录结构与文件字节大小（支持 `max_depth` `1..4`） |
| | `read_file` | 读取 UTF-8 文件全文（`256 KB` 内）或按 `start_line..end_line` 分段读取最大 `16 MB` 文件（`4 MB` 内均返回全文 `SHA256`） |
| | `search_text` | 递归全文/ECMAScript 正则搜索，支持 `file_pattern` 通配符（如 `*.{cpp,hpp}`）并自动遵守 `.gitignore` |
| | `code_outline` | 提取源码中的类、结构体、接口、枚举、函数与方法定义及行号大纲 |
| | `lsp` | 调用项目语言服务获取编译器级 `diagnostics`、`definition`、`references`、`hover` 与 `symbols`（按需启用） |
| | `delegate_research` | 启动独立上下文的只读调研子代理完成跨文件排查，避免撑满主会话上下文 |
| | `delegate_task` / `subagent_output` | 启动可写或自定义子代理（`.deskcode/agents/*.md`），支持 `worktree: true` 独立 Git 分支隔离与 `background: true` 最多 4 路后台并行 |
| **安全代码编辑** | `edit_file` | 校验 `SHA256` 后对文件（最大 `4 MB`）做唯一精确替换，并自动附带最新 LSP 编译诊断 |
| | `multi_edit` | 单次 `SHA256` 校验下对同一文件原子应用 `1..20` 处修改（任一分块不匹配则整体零写入回滚） |
| | `write_file` | 新建或完整替换 UTF-8 文件，自动生成 `.bak` 备份、回读校验并附带 LSP 诊断 |
| **终端与联网** | `run_shell` | 执行非交互式命令（默认 30s，最高 300s），支持实时输出进度与 Linux Landlock 内核写保护；`background: true` 时在后台启动长时间运行的命令 |
| | `shell_output` / `shell_kill` | 读取后台命令的新增输出与退出状态 / 停止后台命令及其子进程（同时最多 8 个，对话结束自动清理） |
| | `web_search` | 关键词联网检索并返回标题、链接与摘要（支持通过 `DESK_SEARCH_URL` 接入自建检索端） |
| | `web_fetch` | 拉取公开 HTTPS 网页（或本机调试 `localhost` HTTP）并提取可读正文，内置私有/内网地址 SSRF 拦截与 DNS 绑定校验 |
| **桌面自动化与视觉** | `desktop_snapshot` | 读取当前桌面应用列表或目标应用的无障碍控件树（最多 500 节点，支持 `filter` 过滤） |
| | `desktop_action` | 触发最新快照中指定控件暴露的原语动作（点击、激活等） |
| | `desktop_set_text` | 设置可编辑桌面控件的文本内容 |
| | `desktop_screenshot` | 截取当前活动窗口或全屏 PNG 图像并按多模态图像块送入模型进行视觉核验 |
| **技能与 MCP 扩展** | `skill` | 按需加载工作区、用户或插件中的 `SKILL.md` 专业技能及配套参考文件（无技能时不占用上下文） |
| | `mcp_<server>_<tool>` / `mcp_read_resource` / `mcp_get_prompt` | 动态挂载本地 `stdio`、远程 `Streamable HTTP`（含 OAuth 2.0 自动续期）的 MCP 工具、只读资源（`resources`）与提示模板（`prompts`） |
| **规划与进度** | `todo_write` | 模型为多步任务维护任务清单（待办 / 进行中 / 已完成），终端实时显示进度 |
| | `submit_plan` | 仅在计划模式提供：提交完整实施方案等待用户批准，批准前不会修改任何文件 |

---

## 对话指令与快捷键

### 斜杠指令与上下文引用

| 指令 / 语法 | 作用 |
|---|---|
| `@关键词` + `Tab` | 模糊搜索并补全工作区文件路径（遵循 `.gitignore`） |
| `@文件` / `@文件:起-止` | 在消息中直接内联附带源码片段或本地图片（`PNG` / `JPEG` / `WebP` / `GIF`） |
| `/` | 打开交互式对话操作面板（切换模型、推理强度、批准模式、MCP、清空上下文等） |
| `/undo` | 一键撤回上一轮任务修改与新建的工作区文件 |
| `/plan [需求]`（或 `Shift+Tab`） | 进入 / 退出计划模式：只调研并提交方案，不修改文件；带需求时直接以计划模式发送 |
| `/go [补充说明]` | 批准当前计划，退出计划模式并开始实施 |
| `/rewind` | 选择回退到某条历史需求之前：可同时撤回对话与文件改动，或仅回退对话 |
| `/cost` · `/price 输入价 输出价 [缓存价] [币种]` | 查看本对话按输入 / 缓存命中 / 输出拆分的词元与估算费用 · 设置当前模型每百万词元单价（`/price off` 清除） |
| `/bg`（或 `Ctrl+B`） | 把正在执行的命令转入后台继续运行，任务不中断 |
| `/compact` | 主动调用当前模型将长对话压缩为高密度工程续接摘要 |
| `/paste [说明]` | 从系统剪贴板读取图片并附带到当前输入（或按 `Ctrl+V`） |
| `/sandbox [net]` · `/notify` | 开启/关闭当前会话的文件系统内核沙箱或子进程禁网隔离（`net`） · 开启/关闭后台任务系统桌面通知 |
| `/commands` · `/skills` · `/plugins` · `/agents` | 查看自定义指令 · 查看 `SKILL.md` 技能 · 查看已安装插件 · 查看内置与自定义子代理 |
| `/approval`（或 `/perm`） | 切换当前对话批准模式（请求批准 / 帮我批准 / 完全访问权限） |
| `/model` · `/reasoning` | 切换模型配置 · 调整模型原生推理强度档位 |
| `/mcp` · `/status` · `/clear` · `/new` · `/help` | 管理 MCP 服务 · 查看会话与词元状态 · 清空上下文 · 新建对话 · 查看快捷键帮助 |

### 键盘与鼠标操作

- **`F2` / `F3`**：快速切换模型 / 调整思考推理强度。
- **`Shift+Tab`**：进入或退出计划模式；**`Ctrl+B`**：把正在执行的命令转入后台（Linux / macOS）。
- **`Alt+Enter`**：在输入框内插入换行；粘贴多行代码自动折叠为摘要块，不会误触发提交。
- **`Ctrl+P` / `Ctrl+N`（或 `Alt+↑` / `Alt+↓`）**：回调当前对话历史发送过的需求。
- **鼠标滚轮 / 右侧滚动条 / `PgUp` `PgDn` / `Ctrl+↑↓`**：上下滚动审阅历史对话与审批面板中的长 Diff；按住 **`Shift` + 鼠标拖动** 选择复制终端文字。
- **`Ctrl+C`**：立即中断当前正在执行的请求或命令；**`Ctrl+D`**：返回对话列表，当前对话在后台继续执行。

---

## 项目级定制与扩展

### 1. 项目约定文件 `DESKCODE.md`
在仓库根目录放置 `DESKCODE.md`（或 `deskcode.md`，同时兼容 `AGENTS.md` 与 `CLAUDE.md`），DeskCode 会在发起请求时自动注入项目构建命令、目录结构与代码规范（设置 `DESK_NO_PROJECT_GUIDE=1` 可临时关闭）。

### 2. 自定义斜杠指令（`.deskcode/commands/*.md`）
在工作区 `.deskcode/commands/`、`.desk-agent/commands/` 或用户目录 `~/.config/desk-agent/commands/` 下创建 Markdown 模板，即可在对话中通过 `/<文件名> [参数]` 调用：

```markdown
<!-- .deskcode/commands/review.md -->
请审查 $ARGUMENTS 的并发安全、错误处理、边界条件与潜在内存泄漏，并给出具体修复建议。
```

### 3. 生命周期钩子（`hooks.json`）
在 `~/.config/desk-agent/hooks.json` 或工作区 `.desk-agent/hooks.json`（工作区级钩子首次运行前会弹窗请求批准）配置 `pre_shell` 与 `post_edit`：

```json
{
  "pre_shell": "if echo \"$DESK_HOOK_CMD\" | grep -q 'git push --force'; then echo '禁止强制推送'; exit 1; fi",
  "post_edit": [
    {
      "pattern": "*.{cpp,hpp}",
      "command": "make -j4 build/desk"
    }
  ]
}
```
- `pre_shell` 返回非零退出码时直接拦截该命令并将输出原因返回给模型；
- `post_edit` 在文件写入成功后自动运行，并将 `hook_exit_code` 与 `hook_output`（如编译报错或单测输出）回传给模型继续自修。

### 4. MCP 服务配置
在主菜单或对话内输入 `/mcp`，支持从文件导入、直接粘贴多行 JSON 或填写远程 URL，同时兼容本地 `stdio` 与远程 `Streamable HTTP`：

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

### 5. Skills 技能与插件生态（兼容 Claude Code 插件）
- **Skills（`SKILL.md`）**：在 `.deskcode/skills/<名称>/SKILL.md`、`.claude/skills/<名称>/SKILL.md` 或用户目录 `~/.config/desk-agent/skills/` 下编写带 YAML Frontmatter（`name` / `description`）的技能说明与参考文件，模型可通过 `skill` 工具按需读取，用户也可直接输入 `/<名称> [需求]` 触发。
- **插件包管理（`desk plugin`）**：一键安装包含 `commands/`、`skills/`、`agents/`、`hooks/hooks.json`（同时兼容 `pre_shell`/`post_edit` 与 Claude `PreToolUse`/`PostToolUse` 格式）和 `.mcp.json` 的插件包或插件市场：

```sh
desk plugin install owner/repo             # 从 GitHub 仓库、Git 地址或本地目录安装插件
desk plugin install owner/marketplace pkg  # 从插件市场安装指定插件
desk plugin list                           # 查看已安装插件及包含的 cmd / skill / mcp / hooks
desk plugin disable pkg && desk plugin enable pkg
desk plugin validate ./my-plugin           # 校验插件目录结构与清单
```

### 6. IDE 接入与 `desk serve` 多端共享守护模式
- **VS Code 扩展（[`editors/vscode/`](editors/vscode/README.md)）**：从 [GitHub Releases](https://github.com/Milorgs/DeskCode/releases) 下载 `deskcode-vscode-1.18.5.vsix` 后执行 `code --install-extension deskcode-vscode-1.18.5.vsix`，即可在侧边栏使用流式对话、四档模式切换、双栏原生 Diff 审阅、权限审批以及右键「选中代码提问 / 修复当前文件报错」。
- **多端共享会话守护进程（`desk serve` + `desk attach`）**：运行 `desk serve --port 7890 --notify` 打开内置 Web 控制台（`http://127.0.0.1:7890`），可在多个终端（`desk attach -p "..."`）、浏览器或移动端隧道上共享同一个实时会话并跨端点击批准。
- **Zed / JetBrains ACP 接入**：运行 `desk acp` 启动标准 ACP v1 服务端。例如在 Zed `settings.json` 中添加：

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

### 7. GitHub Action 与 Python SDK
- **GitHub Action（[`action/`](action/README.md)）**：在仓库 Workflow 中接入 `uses: Milorgs/DeskCode/action@main`，即可在 Issue / PR 评论中输入 `@desk 需求` 自动完成代码修改并创建 PR（含协作者写权限校验与密钥脱敏）。
- **Python SDK（[`sdk/python/`](sdk/python/README.md)）**：`pip install ./sdk/python` 安装零外部依赖的 `deskcode` 包，支持单次 `deskcode.run(...)` / `arun(...)` 与基于 `desk acp` 的流式多轮会话 `DeskAgent` / `AsyncDeskAgent`。

---

## 脚本化与无头自动化（CI / Agent 调用）

当标准输出重定向到文件或管道时，DeskCode 自动将进度与提示输出到 `stderr`，`stdout` 仅输出最终纯文本回复；加上 `--json` 则输出单行结构化 JSON：

```sh
# 仅将最终报告写入文件
desk -p "检查当前项目并列出最关键的三个风险点" > report.md

# 从标准输入管道读取 diff 并自动批准工作区内文件编辑
git diff | desk -p - --approval edit

# 输出机器可读 JSON（含 status / reply / tokens / usage 输入·缓存命中·输出拆分 / elapsed_seconds / tools 调用轨迹）
desk --json -p "运行测试并修复失败用例" --approval edit --max-steps 25

# 计划模式：只调研并把实施方案作为回复输出，不修改任何文件
desk --plan -p "为登录模块增加限流，给出改动方案" > plan.md

# 机器可读环境健康检查
desk doctor --json
```

- **批准档位**：`--approval ask`（默认）｜`--approval edit`（自动批准工作区文件修改与安全只读命令）｜`--approval full`（同 `--yes`，自动批准已启用工具；可选 `--no-sandbox` 关闭沙箱）。
- **退出码约定**：`0` 完成 · `1` 未完成（含非交互终端下需人工审批返回 `approval_required`） · `2` 参数错误 · `130` 中断退出。

---

## Agent 质量跑分

[`bench/`](bench/README.md) 提供可复现的跑分体系：本地离线任务集（`python3 bench/run_local.py`，用 `DESK_*` 环境变量指定模型，隔离配置目录运行并输出通过率、耗时与词元报告）、Terminal-Bench 2.0 / Harbor 适配器（支持 `x86_64` 与 `arm64` 容器）以及多模型榜单聚合脚本（`python3 bench/render_leaderboard.py`）。

---

## 从源码构建与测试

### Linux / macOS
依赖：C++20 编译器（GCC 11+ / Clang 14+）、`make`、`pkg-config`、`libcurl`、`json-c`、`openssl`、`readline`；Linux 桌面无障碍支持另需 `libatspi2.0-dev`。

```sh
make -j"$(nproc)"          # 编译 build/desk
make test                  # 运行单元与集成回归测试套件（约 5310+ 项断言）
make test-mcp-http         # 运行 OpenAI / Anthropic + MCP Streamable HTTP 端到端测试
make sanitize              # 运行 ASan + UBSan 内存与未定义行为检测
./install.sh               # 安装到 ~/.local/bin/desk
```

### Windows (MSVC + CMake + vcpkg)

```powershell
cmake -S . -B build/windows -A x64 "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static -DBUILD_TESTING=ON
cmake --build build/windows --config Release --parallel
ctest --test-dir build/windows -C Release --output-on-failure
```

---

## 相关链接

- **版本发布与安装包下载**：[GitHub Releases](https://github.com/Milorgs/DeskCode/releases)
- **版本更新说明**：[docs/releases/](docs/releases/)
- **早期迭代与工程记录**：[docs/development.md](docs/development.md)
