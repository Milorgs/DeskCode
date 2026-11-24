# DeskCode VS Code Extension (`deskcode-vscode`)

[简体中文](#简体中文) · [English](#english)

---

## 简体中文

**DeskCode AI Coding Agent** 是 DeskCode（`desk acp`）的零外部运行时依赖原生 VS Code 扩展，通过 **Agent Client Protocol (ACP v1)**（基于 `stdio` 的逐行 JSON-RPC 2.0）驱动实时流式编程会话。

### 核心特性

- **侧边栏流式对话面板**：活动栏专属 `DeskCode` 视图，自适应 VS Code 深色 / 浅色 / 高对比度主题（`--vscode-*`）与严格 `nonce` CSP 安全策略。
- **四档模式实时切换**：支持在工具栏或命令面板切换 `ask`（逐项确认）、`edit`（自动批准工作区文件修改与只读命令）、`full`（工作区内全自动）、`plan`（只读调研并输出方案）。
- **工具状态卡片与原生并排 Diff**：实时展示 `read` / `search` / `edit` / `execute` / `think` 工具进度与任务清单（`plan`）；每次文件修改均提供 **View Diff** 按钮，调用 VS Code 原生并排对比视图（基于内存虚拟文档 `deskcode-before:`）。
- **细粒度交互审批**：在对话流中直接提供 `Allow` / `Always allow` / `Reject` 审批按钮；自动对进程错误输出中的 `DESK_API_KEY` 脱敏。
- **编辑器右键联动**：
  - **DeskCode: Ask About Selection** — 将当前选中代码作为内嵌 `resource` 上下文发送提问。
  - **DeskCode: Fix Problems in Active File** — 自动提取当前文件的编译器 / LSP 错误与警告诊断，并附带文件 `resource_link` 一键交由 DeskCode 修复。

### 打包与安装（无需 `npm` / `vsce`）

1. 确保已安装 `desk` 并完成模型配置（在终端运行一次 `desk` 添加模型，或确保 `desk --version` 可用）。
2. 使用纯 Python 3 标准库打包脚本生成 `.vsix` 并安装：

```sh
python3 editors/vscode/package_vsix.py
code --install-extension deskcode-vscode-1.17.0.vsix
```

### 命令列表

| 命令 ID | 标题 | 说明 |
|---|---|---|
| `deskcode.newSession` | `DeskCode: New Session` | 在当前工作区新建对话会话 |
| `deskcode.cancelTurn` | `DeskCode: Stop Current Turn` | 中断正在执行的当前轮次（`session/cancel`） |
| `deskcode.setMode` | `DeskCode: Switch Mode (Ask / Edit / Full / Plan)` | 切换会话批准与规划模式 |
| `deskcode.askSelection` | `DeskCode: Ask About Selection` | 针对编辑器当前选中代码片段提问（含右键菜单） |
| `deskcode.fixDiagnostics` | `DeskCode: Fix Problems in Active File` | 读取当前活动文件的错误/警告诊断并请求修复（含右键菜单） |

### 扩展设置

| 配置项 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `deskcode.executablePath` | `string` | `"desk"` | `desk` 可执行文件路径 |
| `deskcode.defaultMode` | `string` | `"edit"` | 新建会话默认模式：`ask` / `edit` / `full` / `plan` |
| `deskcode.extraArgs` | `string[]` | `[]` | 在 `acp` 子命令前传递给 `desk` 的额外命令行参数 |

### 运行测试

```sh
node editors/vscode/tests/test_vscode.js
```

---

## English

**DeskCode AI Coding Agent** (`deskcode-vscode`) is the zero-runtime-dependency native VS Code extension for DeskCode, speaking the **Agent Client Protocol (`desk acp`, JSON-RPC 2.0 over stdio)** directly to the `desk` binary.

### Features

- **Native Activity Bar Chat View**: Theme-aware (`--vscode-*` dark/light variables) webview guarded by a strict per-render `nonce` Content Security Policy.
- **Four Operating Modes**: Switch seamlessly between `ask`, `edit`, `full`, and `plan` modes from the sidebar dropdown or Command Palette.
- **Live Tool Cards, Plan Checklist & Side-by-Side Diffs**: Watch streaming assistant replies, `todo_write` checklists, and tool executions in real time; click **View Diff** on any edited file to open VS Code's native side-by-side diff (`vscode.diff` backed by an in-memory `deskcode-before:` `TextDocumentContentProvider`).
- **Interactive Tool Approvals & Secret Redaction**: Approve or reject sensitive operations inline (`Allow` / `Always allow` / `Reject`), with automatic `DESK_API_KEY` redaction on agent stderr diagnostics.
- **Editor Context Actions**:
  - **DeskCode: Ask About Selection** (`deskcode.askSelection`) — attaches the selected code range as an ACP embedded `resource` block.
  - **DeskCode: Fix Problems in Active File** (`deskcode.fixDiagnostics`) — collects active errors and warnings from `vscode.languages.getDiagnostics` and sends them alongside a `resource_link` to the active file.

### Quickstart & `.vsix` Installation (No `npm` or `vsce` Required)

1. Install `desk` on your `PATH` (or configure `deskcode.executablePath`) and configure a model (`desk`).
2. Package and install the `.vsix` archive using Python 3's standard library:

```sh
python3 editors/vscode/package_vsix.py
code --install-extension deskcode-vscode-1.17.0.vsix
```

### Settings

| Setting | Type | Default | Description |
|---|---|---|---|
| `deskcode.executablePath` | `string` | `"desk"` | Path to the `desk` binary |
| `deskcode.defaultMode` | `string` | `"edit"` | Default session mode (`ask`, `edit`, `full`, `plan`) |
| `deskcode.extraArgs` | `string[]` | `[]` | Extra CLI arguments passed to `desk` before `acp` |

### Running Tests

```sh
node editors/vscode/tests/test_vscode.js
```
