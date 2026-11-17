#!/bin/sh
# 在隔离沙箱里打开 desk，用于界面测试。
#
#   tools/sandbox.sh                # 打开沙箱窗口（不透明普通 Konsole）
#   tools/sandbox.sh --shot <png>   # 打开沙箱窗口，4 秒后截图并关闭
#   tools/sandbox.sh --clean        # 删除沙箱数据
#
# 隔离方式：
# - 配置目录用 /tmp/desk-sandbox/config（XDG_CONFIG_HOME），不读写 ~/.config/desk-agent，
#   因此不会碰正式的对话记录、MCP 设置和 API 密钥；
# - 模型接口指向本地假服务器 tools/fake_model.py，不联网、不用真实密钥；
# - 工作目录是 /tmp/desk-sandbox/work，命令和文件操作只落在这里。
# 窗口关闭后假服务器随之退出。
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
base=/tmp/desk-sandbox

shot=
if [ "${1:-}" = "--shot" ]; then shot=${2:?缺少截图路径}; fi
if [ "${1:-}" = "--clean" ]; then
    rm -rf "$base"
    echo "已删除沙箱：$base"
    exit 0
fi

mkdir -p "$base/config" "$base/work"
port_file="$base/port"
rm -f "$port_file"

python3 -I "$root/tools/fake_model.py" "$port_file" &
server=$!
trap 'kill "$server" 2>/dev/null || true' EXIT INT TERM

i=0
while [ ! -s "$port_file" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
[ -s "$port_file" ] || { echo "假模型服务器没有启动" >&2; exit 1; }

export XDG_CONFIG_HOME="$base/config"
export DESK_BASE_URL="http://127.0.0.1:$(cat "$port_file")/v1"
export DESK_MODEL=fake-model
export DESK_API_KEY=sandbox-not-a-real-key
export DESK_API_TYPE=openai

cd "$base/work"
# desk-window 会 exec konsole；--separate 让它前台运行，窗口关闭后才返回。
if [ -z "$shot" ]; then
    desk-window --workspace "$base/work"
    exit 0
fi
desk-window --workspace "$base/work" > /dev/null 2>&1 &
window=$!
sleep 4
# spectacle -b 后台  -n 无通知  -a 截活动窗口
spectacle -b -n -a -o "$shot" > /dev/null 2>&1 || true
sleep 1
kill "$window" 2>/dev/null || true
wait "$window" 2>/dev/null || true
