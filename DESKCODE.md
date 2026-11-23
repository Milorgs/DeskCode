# DeskCode

C++20 终端与桌面 AI 编程助手，产品名 `DeskCode`（命令行 `desk`）。原生支持 OpenAI Chat Completions 与 Anthropic Messages 双协议、MCP（stdio + Streamable HTTP）、Linux Landlock 内核命令沙箱与跨平台桌面无障碍自动化。

## 构建与测试

```sh
make -j"$(nproc)"          # 增量构建 build/desk
make test                  # 构建 + 跑 tests/test.cpp（已内置 DESK_NO_ARROW=1）
make test-mcp-http         # MCP HTTP 集成测试（python3 tests/mcp_http.py）
make sanitize              # ASan + UBSan（跑完后执行 make clean 再做普通发布构建）
make install               # 跑 ./install.sh，装到 ~/.local/bin/desk
```

- **测试必须带 `DESK_NO_ARROW=1`**（Makefile 的 `test` 目标已内置，对应 `.github/workflows/test.yml`），用于验证编号菜单输出。
- **当前回归断言基线**：约 **4925–4955** 项（随并发/时序有微小浮动，判断回归以是否存在 `FAIL` 为准）。
- **项目指令与扩展约定**：`DeskCode` 启动任务时自动加载工作区根目录的 `DESKCODE.md` / `deskcode.md`（并兼容 `AGENTS.md`；可通过 `DESK_NO_PROJECT_GUIDE=1` 关闭），支持 `.deskcode/commands/*.md` 与 `.desk-agent/commands/*.md` 自定义斜杠指令以及 `hooks.json`（`pre_shell` / `post_edit`）。

## 真实运行环境与桌面常驻窗口

| 项 | 值 |
|---|---|
| 系统 | Arch Linux，内核 7.3+（支持 Landlock ABI） |
| 桌面 | KDE Plasma / Wayland（`XDG_SESSION_TYPE=wayland`，`WAYLAND_DISPLAY=wayland-1`，`DISPLAY=:0`） |
| 屏幕 | 3072×1920，eDP-1 笔记本内置屏，缩放 2x |
| Locale | `LANG=zh_CN.UTF-8` |
| 桌面常驻挂件 | `~/.local/libexec/desk-agent-window-cli`（由 `desk-agent-window.service` 托管，Konsole profile `DeskAgentClear.profile` `45×19`） |
| 配色约束 | 半透明深色背景下**禁止使用 `\033[2m`（SGR 2 暗淡）**做次要文字区分；统一使用 `src/ui.hpp` 的 `Tone` 语义色（`Tone::Dim` 为 `38;5;247`，`Tone::Rule` 为 `38;5;240`） |
| 字形宽度 | `⏺ ⎿ ❯ ╭ ╮ ╰ ╯ ┃ ⠋ …` 在 `zh_CN.UTF-8` + Noto Sans CJK 回退下实测均为 1 列，汉字 2 列 |

更新本机 CLI 与桌面挂件时，除 `./install.sh`（更新 `~/.local/bin/desk`）外，还需运行 `/usr/bin/bash ~/.local/libexec/desk-agent-window-launch` 更新 `~/.local/libexec/desk-agent-window-cli`，并在无活跃后台子任务时执行 `systemctl --user restart desk-agent-window.service`。

## 测试隔离（必须遵守）

- **严禁触碰正式用户配置**：`~/.config/desk-agent` 存放正式历史记录、MCP 配置与真实 API 密钥。手动或沙箱测试一律使用独立 `XDG_CONFIG_HOME`（如 `/tmp/desk-sandbox/config`）与本地模拟服务 `tools/fake_model.py`。
- **严禁污染系统剪贴板**：桌面自动化测试或窗口输入使用 PTY 脚本或逐键输入，不要通过系统剪贴板注入测试文本；剪贴板贴图与桌面截图单测分别通过 `DESK_CLIPBOARD_IMAGE_FILE` 与 `DESK_DESKTOP_SCREENSHOT_FILE` 注入沙箱文件。
- **密钥零泄漏**：真实 API 密钥不得出现在仓库、提交、日志、截图或 `/tmp` 残留文件中。

## 真机视觉验证

PTY 测试全绿不代表真实终端渲染无误，涉及 UI/排版改动后必须在真实 Konsole 窗口截图核查：
- 使用 `spectacle -b -n -a -o <path>` 后台截取活动窗口（勿加 `--fullscreen`，勿用桌面挂件专用透明 profile 打开测试窗口）。

## 版本与发布

版本号同步维护在四处：`src/desk.hpp`（`VERSION`）、`CMakeLists.txt`、`vcpkg.json`、`README.md`（中英两处），并同步新增中英双语发布说明 `docs/releases/v<版本>.md`。

## 代码约定

- 所有界面可见文案必须经过 `tr(...)` 并在 `src/i18n.cpp` 补齐英文翻译（缺失数为 `0`）。
- 修改既有界面文案前先在 `tests/test.cpp` 中检索固定断言子串。
- 编译开启 `-std=c++20 -Wall -Wextra -Wpedantic -Werror`，且保持 `make sanitize`（ASan + UBSan）零报错。
