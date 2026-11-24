# DeskCode GitHub Action

[中文](#中文) · [English](#english)

---

## 中文

在 Issue / PR 中评论 `@desk 需求`，DeskCode 会在 GitHub Actions runner 上完成修改，并自动开 PR（或推送到当前 PR 分支），最后把回复、改动文件、用量写回评论。

### 快速开始

1. 在仓库 **Settings → Secrets and variables → Actions** 中添加 `DESK_API_KEY`（模型 API 密钥）。
2. 新建 `.github/workflows/deskcode.yml`：

```yaml
name: DeskCode

on:
  issue_comment:
    types: [created]
  pull_request_review_comment:
    types: [created]
  issues:
    types: [opened, assigned, labeled]
  workflow_dispatch:
    inputs:
      prompt:
        description: 交给 DeskCode 的需求
        required: true

permissions:
  contents: write        # 推送分支
  pull-requests: write   # 创建 PR
  issues: write          # 评论与 reaction

concurrency:
  group: deskcode-${{ github.event.issue.number || github.event.pull_request.number || github.run_id }}
  cancel-in-progress: false

jobs:
  desk:
    # 便宜的预过滤；真正的触发、bot 与权限检查在 action 内完成
    if: >-
      github.event_name == 'workflow_dispatch' ||
      contains(github.event.comment.body, '@desk') ||
      contains(github.event.issue.body, '@desk') ||
      contains(github.event.issue.title, '@desk')
    runs-on: ubuntu-latest
    timeout-minutes: 30
    steps:
      - uses: actions/checkout@v4
        with:
          fetch-depth: 0
      - id: desk
        uses: Milorgs/DeskCode/action@main
        with:
          prompt: ${{ github.event.inputs.prompt }}
          api_key: ${{ secrets.DESK_API_KEY }}
          base_url: https://api.openai.com/v1
          model: gpt-4.1
          # api_type: anthropic
          # plan: true            # 只出方案不改代码
      - run: echo "status=${{ steps.desk.outputs.status }} pr=${{ steps.desk.outputs.pr_url }}"
```

### 触发规则

| 事件 | 条件 | 需求来源 | 结果 |
|---|---|---|---|
| `issue_comment`（Issue 上） | 评论含 `trigger` | `trigger` 之后的文字 + Issue 标题/正文作为上下文 | 新分支 `<branch_prefix><issue号>-<短hash>` + PR（`Closes #N`） |
| `issue_comment`（PR 上） / `pull_request_review_comment` | 评论含 `trigger` | 同上（评审评论附文件、行号与 diff hunk） | 同仓库分支：直接提交并推送到 PR 头分支；fork：只评论不推送 |
| `issues` | `opened`/`assigned`/`labeled` 且标题或正文含 `trigger` | `trigger` 之后的文字 | 新分支 + PR |
| `workflow_dispatch` / 其它 | `prompt` 输入（或 `inputs.prompt`）非空 | `prompt` | 新分支 `<branch_prefix><run_id>-<短hash>` + PR |

- 评论/Issue 场景下即使设置了 `prompt`，也仍要求出现 `trigger`；此时 `prompt` 作为固定指令放在评论需求之前。
- 不满足条件时步骤成功结束，`status=skipped`，日志中有 `::notice::` 说明原因。
- 状态不是 `completed`（如 `step_limit`、`failed`）时不提交改动，步骤以失败退出，评论中给出原因。
- `plan: true` 时以 `--plan` 运行，只评论方案，不提交。

### Inputs

| 名称 | 默认 | 说明 |
|---|---|---|
| `prompt` | `""` | 直接给出需求；为空时从事件解析 |
| `trigger` | `@desk` | 触发短语（大小写不敏感，`@desktop` 不会误触发） |
| `api_key` | **必填** | 模型 API 密钥，务必来自 secrets |
| `base_url` | `""` | `DESK_BASE_URL` |
| `model` | `""` | `DESK_MODEL` |
| `api_type` | `openai` | `openai` 或 `anthropic`（`DESK_API_TYPE`） |
| `approval` | `full` | `ask` / `edit` / `full`；runner 是一次性隔离环境 |
| `max_steps` | `40` | 最大步数（0–100，0 不限） |
| `plan` | `false` | 计划模式 |
| `github_token` | `${{ github.token }}` | 评论、推送、开 PR 使用的令牌 |
| `version` | `latest` | 要安装的发布 tag（如 `v1.16.0`；纯数字会自动补 `v`） |
| `create_pr` | `true` | 有改动时建分支并开 PR（PR 评论场景忽略） |
| `branch_prefix` | `desk/` | 新分支前缀 |

### Outputs

| 名称 | 说明 |
|---|---|
| `status` | `completed` / `failed` / `step_limit` / `not_configured` / `interrupted` / `approval_required` / `denied` / `error`（action 自身出错）/ `skipped` |
| `reply` | DeskCode 最终回复（多行 Markdown） |
| `pr_url` | 新建 PR 的链接 |
| `branch` | 接收提交的分支（新分支或 PR 头分支） |
| `tokens` | 总词元数 |
| `cost` | 费用（服务商未返回时为空） |

### 安全说明

- **只信任有写权限的人**：评论/Issue 触发时通过 `gh api repos/{repo}/collaborators/{user}/permission` 检查，只有 `write`/`maintain`/`admin` 才会运行；`github-actions[bot]` 与带有 DeskCode 标记的评论会被忽略，防止自我触发。
- **密钥**：`api_key` 必须放在 secrets 中；action 会 `::add-mask::`，并对日志、评论、PR 正文和 outputs 做脱敏。运行 `desk` 时会移除 `GH_TOKEN`/`GITHUB_TOKEN`/`INPUT_*`/`ACTIONS_*TOKEN` 等环境变量，并使用 `$RUNNER_TEMP` 下的临时 `XDG_CONFIG_HOME`/`XDG_STATE_HOME`（结束后删除）。
- **fork PR 不推送**：来自 fork 的 PR 只检出 `refs/pull/N/head` 运行并评论，不会推送任何内容。不要把本 action 用在 `pull_request_target` 上去自动处理不受信任的代码。
- **提示注入**：Issue/PR 正文可能由无写权限的人撰写并作为上下文传给模型。`actions/checkout` 默认会把令牌保存在 `.git/config` 中，若担心可设置 `persist-credentials: false` 并自行处理推送，或配合 `plan: true` 使用。
- 使用 `GITHUB_TOKEN` 推送/开 PR 不会触发其它 workflow（GitHub 的限制）；如需触发 CI，请传入 PAT 或 GitHub App 令牌到 `github_token`。
- 仓库需在 **Settings → Actions → General** 中允许 “Allow GitHub Actions to create and approve pull requests”，否则 `gh pr create` 会失败。

### 本地测试

```sh
make -j"$(nproc)" build/desk
python3 action/tests/test_action.py
```

测试使用伪造事件、伪 `gh`、本地裸仓库和 `bench/smoke_model.py` 假模型，通过 `DESK_ACTION_DESK_PATH` 跳过下载。

---

## English

Comment `@desk <request>` on an issue or pull request and DeskCode does the work on the Actions runner, opens a pull request (or pushes to the PR branch), and replies with its answer, the changed files and token usage.

### Quick start

1. Add a repository secret `DESK_API_KEY`.
2. Use the workflow shown in the Chinese section above (it is language-neutral): triggers `issue_comment`, `pull_request_review_comment`, `issues` and `workflow_dispatch`, with `permissions: contents: write, pull-requests: write, issues: write`, an `actions/checkout@v4` step and `uses: Milorgs/DeskCode/action@main`.

### Triggers

| Event | Condition | Request | Result |
|---|---|---|---|
| `issue_comment` on an issue | comment contains `trigger` | text after the trigger, with the issue title/body as context | new branch `<branch_prefix><issue>-<sha>` + PR with `Closes #N` |
| `issue_comment` on a PR / `pull_request_review_comment` | comment contains `trigger` | same (review comments add path, line and diff hunk) | same-repo branch: commit and push to the PR head; fork: comment only |
| `issues` | `opened`/`assigned`/`labeled` and title/body contains `trigger` | text after the trigger | new branch + PR |
| `workflow_dispatch` / anything else | `prompt` input (or `inputs.prompt`) | `prompt` | new branch `<branch_prefix><run_id>-<sha>` + PR |

When the conditions are not met the step succeeds with `status=skipped`. Changes are only committed when the status is `completed` and `plan` is false; other statuses fail the step and explain why in the comment.

### Inputs / Outputs

See the tables above: `prompt`, `trigger` (`@desk`), `api_key` (required), `base_url`, `model`, `api_type` (`openai`), `approval` (`full`), `max_steps` (`40`), `plan` (`false`), `github_token` (`github.token`), `version` (`latest`), `create_pr` (`true`), `branch_prefix` (`desk/`). Outputs: `status`, `reply`, `pr_url`, `branch`, `tokens`, `cost`.

### Security

- Only commenters with `write`, `maintain` or `admin` permission (checked via `gh api repos/{repo}/collaborators/{user}/permission`) can start DeskCode; `github-actions[bot]` and DeskCode's own comments are ignored.
- Keep the API key in secrets. It is masked with `::add-mask::` and redacted from logs, comments, PR bodies and outputs. `desk` runs without `GH_TOKEN`/`GITHUB_TOKEN`/`INPUT_*` in its environment and with throw-away XDG directories under `$RUNNER_TEMP`.
- Pull requests from forks are never pushed to. Do not wire this action to `pull_request_target` for untrusted code.
- Issue/PR bodies may come from untrusted users and are passed to the model as context (prompt-injection risk). Consider `persist-credentials: false` or `plan: true` for sensitive repositories.
- Pushes made with `GITHUB_TOKEN` do not trigger other workflows; pass a PAT / GitHub App token as `github_token` if CI must run on DeskCode PRs. Enable “Allow GitHub Actions to create and approve pull requests” in the repository settings.

### Local tests

```sh
make -j"$(nproc)" build/desk
python3 action/tests/test_action.py
```
