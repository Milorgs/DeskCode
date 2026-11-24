# DeskCode Python SDK (`deskcode`)

[中文](#中文) · [English](#english)

---

## 中文

`deskcode` 是 DeskCode（`desk`）的零外部依赖 Python SDK（仅使用 Python 标准库，支持 Python 3.9+），提供两种调用方式：

1. **单次运行（`run` / `arun`）**：封装 `desk --json -p -`，返回结构化的 `RunResult`（状态、回复、词元拆分、费用、工具轨迹），并对异常输出中的 API Key 自动脱敏。
2. **流式会话（`DeskAgent` / `AsyncDeskAgent`）**：通过 **Agent Client Protocol（`desk acp`）** 维护常驻会话，实时接收流式文本分片、工具调用、文件 Diff、任务清单（`plan`）事件，并通过 Python 回调控制每次工具权限审批或中途取消。

### 安装

```sh
pip install ./sdk/python
# 或可编辑安装：
pip install -e ./sdk/python
```

（需已安装 `desk` 并位于 `PATH`，或通过 `desk_path="/path/to/desk"` 指定。）

### 1. 单次无头调用（`run` / `arun`）

```python
import deskcode

res = deskcode.run(
    "运行测试并修复失败项",
    cwd="/path/to/project",
    approval="edit",          # "ask" | "edit" | "full"
    max_steps=30,
    # 可选：覆盖当前配置的模型参数
    # model="deepseek-chat",
    # base_url="https://api.deepseek.com",
    # api_key="sk-...",
)

if res.ok:
    print(res.reply)
    print(f"tokens={res.tokens} (in={res.usage.input}, out={res.usage.output}, cache={res.usage.cache_read})")
else:
    print("未完成:", res.status, res.error)
```

异步版本参数完全一致：

```python
res = await deskcode.arun("梳理项目架构", cwd=".", plan=True)
```

### 2. ACP 流式多轮会话（`DeskAgent` / `AsyncDeskAgent`）

```python
from deskcode import DeskAgent, TextChunk, ToolCall, ToolCallUpdate, PlanUpdate, allow_reads_only

def approve(req):
    # 自定义审批策略：允许只读与文件编辑，拦截危险命令
    if req.tool_call.kind in ("read", "search", "think", "edit"):
        return req.option("allow_once", "allow_always")
    return req.option("reject_once")

with DeskAgent(on_permission=approve) as agent:
    session = agent.new_session(cwd=".", mode="ask")  # "ask" | "edit" | "full" | "plan"

    for event in session.prompt("检查 src/ 下的潜在空指针问题并修复", resources=["README.md"]):
        if isinstance(event, TextChunk):
            print(event.text, end="", flush=True)
        elif isinstance(event, ToolCall):
            print(f"\n[工具] {event.title} ({event.kind})")
        elif isinstance(event, ToolCallUpdate) and event.diffs:
            for d in event.diffs:
                print(f"\n[修改文件] {d.path}")
        elif isinstance(event, PlanUpdate):
            for item in event.entries:
                print(f"  - [{item.status}] {item.content}")

    # 同一会话继续追问（保留上下文）
    turn = session.send("再补一个对应的回归测试")
    print("\n完成原因:", turn.stop_reason)
```

内置权限策略：`allow_all`、`reject_all`（默认）、`allow_reads_only`。同样提供 `AsyncDeskAgent`（`async with AsyncDeskAgent(...) as agent:`）。

### 本地测试

```sh
make -j"$(nproc)" build/desk
PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=sdk/python/src python3 -m unittest discover -s sdk/python/tests -v
```

---

## English

`deskcode` is the zero-dependency Python SDK (Python 3.9+ standard library only) for DeskCode (`desk`), offering both:

1. **One-shot headless runs (`run` / `arun`)** wrapping `desk --json -p -` into a typed `RunResult` (`status`, `reply`, split `usage`, `tools` trace) with automatic API-key redaction on errors.
2. **Interactive streaming sessions (`DeskAgent` / `AsyncDeskAgent`)** over the **Agent Client Protocol (`desk acp`)**, streaming `TextChunk`, `ToolCall`, `ToolCallUpdate` (including `DiffContent` and LSP diagnostics summaries), `PlanUpdate`, custom `on_permission` callbacks, and `session.cancel()`.

### Install

```sh
pip install ./sdk/python
```

### One-shot run

```python
import deskcode

result = deskcode.run("Run the test suite and fix failures", cwd=".", approval="edit")
if result.ok:
    print(result.reply)
```

### Streaming ACP session

```python
from deskcode import DeskAgent, TextChunk, allow_all

with DeskAgent(on_permission=allow_all) as agent:
    session = agent.new_session(cwd=".", mode="edit")
    for event in session.prompt("Summarize this repository", resources=["README_EN.md"]):
        if isinstance(event, TextChunk):
            print(event.text, end="", flush=True)
```

### Tests

```sh
make -j"$(nproc)" build/desk
PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=sdk/python/src python3 -m unittest discover -s sdk/python/tests -v
```
