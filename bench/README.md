# DeskCode Bench · Agent 质量跑分

本目录为 DeskCode（`desk`）提供可复现的 Agent 质量评测：

- **本地离线任务集**（`local/`）——10 个小而有区分度的编程任务，隐藏校验，几分钟跑完，适合每次发版前回归；
- **Terminal-Bench 2.0 / Harbor 适配器**（`harbor/`）——在官方容器化基准上与其他 Agent 横向对比。

> [!IMPORTANT]
> 严禁把真实 API 密钥写进仓库、命令历史文件、结果文件或截图。运行器只从**当前命令的环境变量**读取 `DESK_*`，
> 每次运行都使用临时 `HOME` / `XDG_CONFIG_HOME` / `XDG_STATE_HOME`，不会读写 `~/.config/desk-agent`，
> 并在写出结果前对密钥做脱敏。

## 1. 本地任务集

| 任务 | 类别 | 难度 | 考察点 |
|---|---|---|---|
| `py-fix-failing-tests` | python-debug | easy | 根据失败的单测修复 3 个 bug（不许改测试） |
| `c-fix-build` | c-build | easy | 让 C 项目在 `-Wall -Wextra -Werror` 下编译、链接通过 |
| `c-memory-bugs` | c-memory | medium | 修复越界、错误 `realloc` 尺寸、释放后使用等，ASan/UBSan 校验 |
| `cli-add-options` | feature | medium | 给 CLI 加 `--top N` / `--ignore-case`，含参数校验与文档 |
| `refactor-table-format` | refactor | medium | 抽取重复格式化逻辑为新模块，输出逐字节不变 |
| `spec-parse-duration` | implement-spec | medium | 按规格实现 `parse_duration` / `format_duration`（测试只在 check.sh 内） |
| `rename-across-files` | multi-file-edit | easy | 跨代码/测试/文档重命名函数与模块，不留别名 |
| `csv-region-report` | shell-data | medium | 写脚本汇总 CSV：引号、列乱序、CRLF、并列排序 |
| `config-ini-update` | config | easy | 修改 INI 配置并保留注释，不得误改示例文件 |
| `git-feature-branch` | git | medium | 新建分支、单次规范提交、切回 main 且工作区干净 |

每个任务目录 `local/tasks/<name>/`：

- `task.json`：`id`、`title`、`category`、`difficulty`、`timeout_seconds`、可选 `max_steps`；
- `instruction.md`：交给 Agent 的需求（英文）；
- `setup/`：初始工作区，原样复制；
- `check.sh`：在工作区内运行，退出码 0 即通过。全部离线、确定性，只依赖 bash / python3 / cc / make / git；
  隐藏测试写在脚本内部、在临时目录中执行，构建类检查在工作区副本里进行，**不信任 Agent 的自述**。

`local/solutions/<name>/solve.sh` 是参考解，仅供 `--self-test` 使用，运行器默认不会用到。

### 用法

```sh
make -j"$(nproc)"                                  # 生成 build/desk
python3 bench/run_local.py --list                  # 列出任务
python3 bench/run_local.py --self-test             # 校验任务本身：初始失败、参考解通过（不调用 desk）
python3 bench/smoke_test.py                        # 用本地假模型端到端冒烟（不联网）

# 真实模型（变量只对这一条命令生效；不要把密钥写进脚本或提交）
DESK_BASE_URL=https://api.example.com/v1 DESK_MODEL=<model> DESK_API_TYPE=openai \
DESK_API_KEY="$(secret-tool lookup service deskcode)" \
python3 bench/run_local.py --label <model> --repeat 3 --jobs 4
```

常用参数：`--tasks 'c-*,git-*'`（逗号分隔、支持通配）、`--repeat N`、`--jobs N`、`--timeout S`、`--max-steps N`、
`--label`、`--out PATH`（默认 `bench/results/<UTC 时间戳>-<label>.json`，该目录已被 git 忽略）、
`--keep`（保留临时工作区便于排查）、`--desk-arg=--no-sandbox`（透传额外参数给 desk）。

每次运行：复制 `setup/` 到 `tempfile.mkdtemp(prefix="desk-bench-")`，`git init` 并提交初始状态（分支 `main`），
以 `desk --json -p <instruction> --approval full --workspace <dir> [--max-steps N]` 运行（标准输入为 `/dev/null`，
独立进程组，超时整组杀掉），然后执行 `check.sh`，统计 `git diff` 后清理临时目录。

### 结果解读

终端会打印 Markdown 汇总表；JSON 结果文件包含 `summary`（总体与每任务）和 `results`（每次运行）：

| 字段 | 含义 |
|---|---|
| `passed` | `check.sh` 是否通过——**唯一的得分依据** |
| `status` | desk 自报状态：`completed` / `step_limit` / `failed` / `interrupted` …；超时记为 `timeout`（原状态在 `desk_status`） |
| `tokens` / `usage` | desk 统计的词元；若 desk 输出可选的 `usage`（`input` / `output` / `cache_read` / `cost`）会一并记录 |
| `tool_calls` / `tool_failures` / `tools_by_name` | 工具调用次数、失败次数与分布 |
| `wall_seconds` / `elapsed_seconds` | 运行器测得的墙钟时间 / desk 自报用时 |
| `diff` | 相对初始提交的改动文件数、增删行数、新提交数 |
| `check.output` | 校验输出末尾，便于定位失败原因 |

- `status=completed` 但 `passed=false`：Agent 以为做完了，实际没满足隐藏校验——最值得关注的回归信号。
- 模型输出有随机性，比较不同版本/模型时建议 `--repeat 3` 以上并看通过率均值；
  对比时固定 `--max-steps`、`--timeout` 与 desk 版本（结果文件里都有记录）。

## 2. Terminal-Bench 2.0（Harbor）

`harbor/deskcode_agent.py` 中的 `DeskCodeAgent` 是 Harbor 的“已安装 Agent”（`BaseInstalledAgent`）：

1. `install()`：上传并以 root 执行 `harbor/install-deskcode.sh`——安装运行库（apt/dnf/yum），下载
   `https://github.com/Milorgs/DeskCode/releases/latest/download/desk-agent-linux-x64.tar.gz`，
   用包内 `install.sh --prefix /usr/local` 安装，并用 `ldd` 检查依赖；
2. `run()`：把任务指令上传为文件，在任务工作目录执行
   `desk --json -p - --approval full --no-sandbox --workspace "$PWD" < instruction.md`，
   结果写入 `/logs/agent/desk-result.json`，并把 tokens / 工具调用等写进 Harbor 的 `AgentContext.metadata`。

```sh
pip install harbor            # 需要 Docker；Harbor >= 0.6（按 v0.27.0 源码实现）
export DESK_BASE_URL=https://api.example.com/v1 DESK_MODEL=<model> DESK_API_TYPE=openai
read -rs DESK_API_KEY && export DESK_API_KEY     # 交互输入，避免进入 shell 历史

# 在仓库根目录运行；Harbor 用 importlib 按模块路径导入，不会自动把当前目录加入 sys.path，需 PYTHONPATH
PYTHONPATH="$PWD" harbor run -d terminal-bench@2.0 \
  --agent bench.harbor.deskcode_agent:DeskCodeAgent \
  --n-concurrent 4 \
  --ae DESK_BASE_URL="$DESK_BASE_URL" --ae DESK_MODEL="$DESK_MODEL" \
  --ae DESK_API_TYPE="$DESK_API_TYPE" --ae DESK_API_KEY="$DESK_API_KEY" \
  --ak max_steps=100

# 先小规模试跑：--n-tasks 5 或 --include-task-name <name>
```

- 老版本 Harbor 用 `--agent-import-path bench.harbor.deskcode_agent:DeskCodeAgent`（新版本中已标记为弃用）。
- 可选 `--ak`：`release_url=<tar.gz>`（测试未发布构建）、`max_steps=N`、`workdir=/app`、
  `strict_exit=true`（desk 非 0 退出即判 Agent 失败；默认交给 verifier 判定）、`extra_args="--lang en"`、`sandbox=true`。
- **费用**：Terminal-Bench 2.0 约 90 个任务，每个任务可能消耗数十万词元；先用 `--n-tasks` 小规模估算成本。
- **并发**：`--n-concurrent` 受本机 Docker 资源和模型服务限流约束，建议从 2–4 开始；遇到 429 可配合
  `--max-retries` 重试。
- **网络**：容器内需要能访问 GitHub Release（安装）和模型接口（运行）；网络受限的环境需要放行对应主机。
- **兼容性**：预编译二进制要求 x86_64 + glibc ≥ 2.35（Ubuntu 22.04+/Debian 12+），Alpine(musl) 镜像不支持。
- **密钥**：`--ae` 的值会出现在本机进程列表中；不要把带密钥的命令写进脚本或提交，Harbor 的作业目录（`jobs/`）也不要提交。

---

## English summary

**Local suite.** `bench/local/tasks/*` holds 10 offline tasks (Python debugging, C build and
memory bugs, CLI feature, refactor, spec implementation with hidden tests, multi-file rename,
CSV processing, INI config edit, git branching). Each task has `task.json`, `instruction.md`,
an optional `setup/` workspace and a deterministic `check.sh` (exit 0 = pass; hidden tests live
inside the script). `python3 bench/run_local.py --self-test` proves every check fails on the
initial state and passes with the reference solution in `local/solutions/`.
`python3 bench/smoke_test.py` runs the whole pipeline against a scripted local fake model.

Run against a real model with `DESK_BASE_URL`, `DESK_MODEL`, `DESK_API_KEY` and `DESK_API_TYPE`
set for that command only: `python3 bench/run_local.py --label <model> --repeat 3 --jobs 4`.
Every run uses a fresh temp dir with private `HOME`/XDG directories (never
`~/.config/desk-agent`), a git baseline commit, `desk --json --approval full`, a process-group
timeout, then `check.sh`. Output: a Markdown table and a JSON report in `bench/results/`
(git-ignored) with pass/fail, status, tokens, optional `usage`, tool calls and diff stats.
Secrets are redacted before anything is written. Only `passed` counts as the score.

**Terminal-Bench 2.0.** `bench/harbor/deskcode_agent.py` implements a Harbor
`BaseInstalledAgent` (`install()` downloads the GitHub release and runs its `install.sh`;
`run()` executes `desk --json -p - --approval full --no-sandbox --workspace "$PWD"`). Run it with
`PYTHONPATH="$PWD" harbor run -d terminal-bench@2.0 --agent bench.harbor.deskcode_agent:DeskCodeAgent --ae DESK_...=...`.
Mind the cost (≈90 tasks), keep `--n-concurrent` modest, make sure the model endpoint is
reachable from the containers, and never commit keys or Harbor job directories.
