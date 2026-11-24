#!/usr/bin/env python3
"""Generate crisp animated SVG showcases and an optimized GIF for DeskCode READMEs."""
from __future__ import annotations

import pathlib
import shutil
import subprocess
import tempfile
import zlib
import struct

ROOT = pathlib.Path(__file__).resolve().parents[1]
ASSETS = ROOT / "docs" / "assets"

TERMINAL_SVG = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 920 500" width="920" height="500" role="img" aria-label="DeskCode terminal workflow demo">
  <defs>
    <style>
      .bg { fill: #12151c; }
      .bar { fill: #1b202b; }
      .border { stroke: #2c3344; stroke-width: 1.5; fill: none; }
      .composer { fill: #171c26; stroke: #3b465e; stroke-width: 1; }
      .txt { font-family: 'JetBrains Mono', 'Fira Code', 'SFMono-Regular', Consolas, 'Noto Sans Mono CJK SC', monospace; font-size: 13.5px; }
      .title { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; font-size: 12.5px; fill: #9aa5be; font-weight: 600; }
      .plain { fill: #e6edf3; }
      .dim { fill: #939db0; }
      .accent { fill: #6f9fff; font-weight: 600; }
      .ok { fill: #7ee787; }
      .warn { fill: #f2cc60; }
      .err { fill: #ff7b72; }
      .badge { fill: #1f2b42; stroke: #44659e; stroke-width: 1; }
      .s1 { opacity: 0; animation: show1 14s infinite; }
      .s2 { opacity: 0; animation: show2 14s infinite; }
      .s3 { opacity: 0; animation: show3 14s infinite; }
      .s4 { opacity: 0; animation: show4 14s infinite; }
      .s5 { opacity: 0; animation: show5 14s infinite; }
      @keyframes show1 { 0%,4% { opacity: 0; } 7%,94% { opacity: 1; } 98%,100% { opacity: 0; } }
      @keyframes show2 { 0%,20% { opacity: 0; } 24%,94% { opacity: 1; } 98%,100% { opacity: 0; } }
      @keyframes show3 { 0%,40% { opacity: 0; } 44%,94% { opacity: 1; } 98%,100% { opacity: 0; } }
      @keyframes show4 { 0%,60% { opacity: 0; } 64%,94% { opacity: 1; } 98%,100% { opacity: 0; } }
      @keyframes show5 { 0%,76% { opacity: 0; } 80%,94% { opacity: 1; } 98%,100% { opacity: 0; } }
    </style>
  </defs>

  <rect width="920" height="500" rx="12" class="bg"/>
  <path d="M12 0 H908 A12 12 0 0 1 920 12 V38 H0 V12 A12 12 0 0 1 12 0 Z" class="bar"/>
  <circle cx="22" cy="19" r="6" fill="#ff5f56"/>
  <circle cx="42" cy="19" r="6" fill="#ffbd2e"/>
  <circle cx="62" cy="19" r="6" fill="#27c93f"/>
  <text x="460" y="24" text-anchor="middle" class="title">DeskCode 1.17.0 — Parallel Git Worktrees · LSP Auto-Diagnostics · Plan Mode</text>
  <rect x="0.75" y="0.75" width="918.5" height="498.5" rx="12" class="border"/>

  <!-- Header -->
  <text x="24" y="66" class="txt accent">DeskCode 1.17.0</text>
  <text x="160" y="66" class="txt dim">· claude-sonnet-4 · ~/srv/gateway · Landlock sandbox ✓ · 2 background chats</text>

  <!-- Step 1: User request -->
  <g class="s1">
    <text x="24" y="98" class="txt accent">❯</text>
    <text x="44" y="98" class="txt plain">并行用两个独立 worktree 子代理重构限流中间件与补齐单元测，合并前过一遍 clangd 诊断。</text>
  </g>

  <!-- Step 2: Checklist + parallel worktree subagents -->
  <g class="s2">
    <text x="24" y="130" class="txt accent">⏺ 更新任务清单 0/3</text>
    <text x="40" y="152" class="txt accent">  [~] 并行启动两个 git worktree 子代理（重构限流模块 / 编写回归单测）</text>
    <text x="40" y="172" class="txt dim">  [ ] 合并两个子代理分支并核查 clangd LSP 诊断</text>
    <text x="40" y="192" class="txt dim">  [ ] 运行全量 CTest 并汇总结果</text>
    <text x="24" y="220" class="txt accent">⏺ 委派子代理(rate_limiter.cpp: 令牌桶重构 · worktree · background)</text>
    <text x="40" y="240" class="txt dim">  ⎿  完成 · 后台运行中 agent-1 (desk/coder-7f2a9c10) · 0.1s</text>
    <text x="24" y="264" class="txt accent">⏺ 委派子代理(tests/limiter_test.cpp: 并发边界单测 · worktree · background)</text>
    <text x="40" y="284" class="txt dim">  ⎿  完成 · 后台运行中 agent-2 (desk/coder-3e8b1d04) · 0.1s</text>
  </g>

  <!-- Step 3: Harvest subagent branches & merge -->
  <g class="s3">
    <text x="24" y="312" class="txt accent">⏺ 查看子代理结果(agent-1, agent-2)</text>
    <text x="40" y="332" class="txt ok">  ⎿  完成 · 2 个 worktree 分支已提交就绪（+148 −62 行，零冲突合并至 main） · 2.4s</text>
  </g>

  <!-- Step 4: LSP diagnostics caught & fixed automatically -->
  <g class="s4">
    <text x="24" y="360" class="txt accent">⏺ 修改文件(src/rate_limiter.cpp)</text>
    <text x="40" y="380" class="txt ok">  ⎿  完成 · 已回读核对 · clangd: no errors or warnings · 0.3s</text>
  </g>

  <!-- Step 5: Final reply -->
  <g class="s5">
    <text x="24" y="410" class="txt ok">●</text>
    <text x="44" y="410" class="txt plain">已完成令牌桶限流重构与 14 项并发回归测试，`clangd` 零告警，`ctest` 14/14 全部通过。</text>
    <text x="44" y="430" class="txt dim">用时 3.8s · 输入 4.2k · 缓存命中 18.6k · 输出 0.9k 词元 · ≈$0.031 · 完成</text>
  </g>

  <!-- Pinned Composer -->
  <rect x="18" y="450" width="884" height="34" rx="6" class="composer"/>
  <text x="32" y="472" class="txt accent">❯</text>
  <text x="52" y="472" class="txt dim">/ 命令面板 · Shift+Tab 计划模式 · /rewind 回退 · /agents 子代理 · @+Tab 补全文件</text>
  <rect x="748" y="456" width="144" height="22" rx="4" class="badge"/>
  <text x="820" y="471" text-anchor="middle" class="txt accent" style="font-size:11.5px">批准：自动批准编辑</text>
</svg>
"""

GUI_SVG = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 920 420" width="920" height="420" role="img" aria-label="DeskCode native desktop GUI accessibility automation demo">
  <defs>
    <style>
      .bg { fill: #0f131a; }
      .panel { fill: #161b24; stroke: #2c3547; stroke-width: 1.5; }
      .win { fill: #1e2430; stroke: #3d4a63; stroke-width: 1.5; }
      .input { fill: #121720; stroke: #3c485e; stroke-width: 1; }
      .input-hi { fill: #152238; stroke: #6f9fff; stroke-width: 1.5; }
      .btn { fill: #2b59c3; }
      .btn-done { fill: #238636; }
      .txt { font-family: 'JetBrains Mono', 'Fira Code', Consolas, 'Noto Sans Mono CJK SC', monospace; font-size: 12.8px; }
      .ui { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; font-size: 13px; fill: #dce3ee; }
      .dim { fill: #8b96ab; }
      .accent { fill: #6f9fff; font-weight: 600; }
      .ok { fill: #7ee787; }
      .plain { fill: #e6edf3; }
      .g1 { opacity: 0; animation: gshow1 12s infinite; }
      .g2 { opacity: 0; animation: gshow2 12s infinite; }
      .g3 { opacity: 0; animation: gshow3 12s infinite; }
      .g4 { opacity: 0; animation: gshow4 12s infinite; }
      @keyframes gshow1 { 0%,6% { opacity: 0; } 10%,92% { opacity: 1; } 97%,100% { opacity: 0; } }
      @keyframes gshow2 { 0%,28% { opacity: 0; } 33%,92% { opacity: 1; } 97%,100% { opacity: 0; } }
      @keyframes gshow3 { 0%,52% { opacity: 0; } 57%,92% { opacity: 1; } 97%,100% { opacity: 0; } }
      @keyframes gshow4 { 0%,74% { opacity: 0; } 79%,92% { opacity: 1; } 97%,100% { opacity: 0; } }
    </style>
  </defs>

  <rect width="920" height="420" rx="12" class="bg"/>

  <!-- Left: DeskCode Terminal -->
  <rect x="14" y="14" width="510" height="392" rx="10" class="panel"/>
  <text x="30" y="42" class="txt accent">DeskCode Terminal · Native GUI Accessibility</text>
  <text x="30" y="64" class="txt dim">AT-SPI (Linux) · UI Automation (Win) · System Events (macOS)</text>

  <text x="30" y="96" class="txt accent">❯</text>
  <text x="48" y="96" class="txt plain">把 config.json 的端口和网关填进「Deploy Console」窗口并点击 Apply。</text>

  <g class="g1">
    <text x="30" y="130" class="txt accent">⏺ 读取桌面(app="Deploy Console")</text>
    <text x="44" y="150" class="txt dim">  ⎿  完成 · 12 个无障碍控件 (@e3 Host, @e5 Port, @e8 Apply) · 0.1s</text>
  </g>

  <g class="g2">
    <text x="30" y="182" class="txt accent">⏺ 操作桌面(@e3 → "10.24.0.8", @e5 → "8443")</text>
    <text x="44" y="202" class="txt dim">  ⎿  完成 · desktop_set_text 已写入可编辑控件 · 0.2s</text>
  </g>

  <g class="g3">
    <text x="30" y="234" class="txt accent">⏺ 操作桌面(@e8 action="click") + 截取桌面</text>
    <text x="44" y="254" class="txt ok">  ⎿  完成 · 已点击 Apply 并附带活动窗口截图多模态核验 · 0.4s</text>
  </g>

  <g class="g4">
    <text x="30" y="292" class="txt ok">●</text>
    <text x="48" y="292" class="txt plain">已填入网关 `10.24.0.8:8443` 并触发 Apply，截图确认状态已变为 Active。</text>
  </g>

  <!-- Right: Native Desktop App Window -->
  <rect x="542" y="14" width="364" height="392" rx="10" class="win"/>
  <rect x="542" y="14" width="364" height="34" rx="10" fill="#273042"/>
  <text x="724" y="36" text-anchor="middle" class="ui" style="font-weight:600">Deploy Console (Native Desktop App)</text>

  <text x="566" y="82" class="ui dim">Gateway Host  [@e3]</text>
  <rect x="566" y="92" width="316" height="34" rx="6" class="input"/>
  <text x="578" y="114" class="txt dim">127.0.0.1</text>
  <g class="g2">
    <rect x="566" y="92" width="316" height="34" rx="6" class="input-hi"/>
    <text x="578" y="114" class="txt plain">10.24.0.8</text>
  </g>

  <text x="566" y="156" class="ui dim">TLS Listen Port  [@e5]</text>
  <rect x="566" y="166" width="316" height="34" rx="6" class="input"/>
  <text x="578" y="188" class="txt dim">8080</text>
  <g class="g2">
    <rect x="566" y="166" width="316" height="34" rx="6" class="input-hi"/>
    <text x="578" y="188" class="txt plain">8443</text>
  </g>

  <rect x="566" y="226" width="316" height="38" rx="6" class="btn"/>
  <text x="724" y="250" text-anchor="middle" class="ui" style="font-weight:600;fill:#ffffff">Apply Configuration  [@e8]</text>

  <g class="g3">
    <rect x="566" y="226" width="316" height="38" rx="6" class="btn-done"/>
    <text x="724" y="250" text-anchor="middle" class="ui" style="font-weight:600;fill:#ffffff">✓ Configuration Applied</text>
  </g>

  <rect x="566" y="288" width="316" height="92" rx="6" fill="#131822" stroke="#2c3547"/>
  <text x="580" y="314" class="txt dim">Status: Idle (port 8080)</text>
  <g class="g3">
    <rect x="566" y="288" width="316" height="92" rx="6" fill="#12211c" stroke="#2ea043"/>
    <text x="580" y="316" class="txt ok">● Status: Active (TLS 10.24.0.8:8443)</text>
    <text x="580" y="340" class="txt dim">Verified via desktop_screenshot</text>
    <text x="580" y="362" class="txt dim">Zero browser/Playwright dependency</text>
  </g>
</svg>
"""


def make_gif_with_ffmpeg(out_gif: pathlib.Path) -> None:
    """Also build a compact looping GIF via ffmpeg drawtext/lavfi if ffmpeg is installed."""
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        return
    # Build 5 clean PNG frames with pure Python zlib/struct (zero Pillow dependency) or ffmpeg filter
    # Use a crisp 5x7 bitmap font renderer in pure Python so the GIF has zero fontconfig dependency!
    w, h = 680, 260
    bg = (18, 21, 28)
    bar = (27, 32, 43)
    acc = (111, 159, 255)
    ok = (126, 231, 135)
    fg = (230, 237, 243)
    dim = (147, 157, 176)

    FONT = {
        " ": [0,0,0,0,0,0,0],
        ">": [16,8,4,2,4,8,16],
        ".": [0,0,0,0,0,12,12],
        ":": [0,12,12,0,12,12,0],
        "-": [0,0,0,31,0,0,0],
        "/": [1,2,4,8,16,0,0],
        "+": [0,4,4,31,4,4,0],
        "(": [2,4,8,8,8,4,2],
        ")": [8,4,2,2,2,4,8],
        "[": [14,8,8,8,8,8,14],
        "]": [14,2,2,2,2,2,14],
        "*": [0,21,14,31,14,21,0],
        "0": [14,17,19,21,25,17,14],
        "1": [4,12,4,4,4,4,14],
        "2": [14,17,1,6,8,16,31],
        "3": [31,2,4,6,1,17,14],
        "4": [2,6,10,18,31,2,2],
        "5": [31,16,30,1,1,17,14],
        "6": [6,8,16,30,17,17,14],
        "7": [31,1,2,4,8,8,8],
        "8": [14,17,17,14,17,17,14],
        "9": [14,17,17,15,1,2,12],
    }

    def write_png(path: pathlib.Path, rgb: bytearray) -> None:
        raw = bytearray()
        stride = w * 3
        for y in range(h):
            raw.append(0)
            raw.extend(rgb[y * stride:(y + 1) * stride])
        def chunk(tag: bytes, data: bytes) -> bytes:
            return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
        png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
        path.write_bytes(png)

    # Simple 5x7 uppercase/lowercase ASCII bitmap via basic strokes
    ALPHA = {
        "A":[14,17,17,31,17,17,17],"B":[30,17,17,30,17,17,30],"C":[14,17,16,16,16,17,14],
        "D":[28,18,17,17,17,18,28],"E":[31,16,16,30,16,16,31],"F":[31,16,16,30,16,16,16],
        "G":[14,17,16,23,17,17,15],"H":[17,17,17,31,17,17,17],"I":[14,4,4,4,4,4,14],
        "J":[7,2,2,2,18,18,12],"K":[17,18,20,24,20,18,17],"L":[16,16,16,16,16,16,31],
        "M":[17,27,21,21,17,17,17],"N":[17,25,21,19,17,17,17],"O":[14,17,17,17,17,17,14],
        "P":[30,17,17,30,16,16,16],"Q":[14,17,17,17,21,18,13],"R":[30,17,17,30,20,18,17],
        "S":[15,16,16,14,1,1,30],"T":[31,4,4,4,4,4,4],"U":[17,17,17,17,17,17,14],
        "V":[17,17,17,17,17,10,4],"W":[17,17,17,21,21,27,17],"X":[17,17,10,4,10,17,17],
        "Y":[17,17,10,4,4,4,4],"Z":[31,1,2,4,8,16,31],"_": [0,0,0,0,0,0,31],
    }
    FONT.update(ALPHA)

    def draw_text(buf: bytearray, x: int, y: int, text: str, color: tuple[int,int,int], scale: int = 2) -> None:
        cx = x
        for ch in text:
            rows = FONT.get(ch) or FONT.get(ch.upper()) or FONT[" "]
            for ry, bits in enumerate(rows):
                for rx in range(5):
                    if bits & (1 << (4 - rx)):
                        for sy in range(scale):
                            for sx in range(scale):
                                px, py = cx + rx * scale + sx, y + ry * scale + sy
                                if 0 <= px < w and 0 <= py < h:
                                    idx = (py * w + px) * 3
                                    buf[idx:idx+3] = bytes(color)
            cx += 6 * scale

    steps = [
        [("DESKCODE 1.17.0  -  C++20 NATIVE AI CODING AGENT", acc),
         ("> Refactor limiter + add tests in parallel worktrees", fg)],
        [("DESKCODE 1.17.0  -  C++20 NATIVE AI CODING AGENT", acc),
         ("> Refactor limiter + add tests in parallel worktrees", fg),
         ("* delegate_task(agent-1, worktree=true, background=true)", acc),
         ("* delegate_task(agent-2, worktree=true, background=true)", acc)],
        [("DESKCODE 1.17.0  -  C++20 NATIVE AI CODING AGENT", acc),
         ("> Refactor limiter + add tests in parallel worktrees", fg),
         ("* delegate_task(agent-1, worktree=true, background=true)", acc),
         ("* delegate_task(agent-2, worktree=true, background=true)", acc),
         ("* subagent_output -> merged 2 branches (+148 -62)", ok)],
        [("DESKCODE 1.17.0  -  C++20 NATIVE AI CODING AGENT", acc),
         ("> Refactor limiter + add tests in parallel worktrees", fg),
         ("* delegate_task(agent-1, worktree=true, background=true)", acc),
         ("* delegate_task(agent-2, worktree=true, background=true)", acc),
         ("* subagent_output -> merged 2 branches (+148 -62)", ok),
         ("* lsp clangd: 0 errors, 0 warnings  |  ctest 14/14 PASS", ok),
         ("Done in 3.8s  (cache hit 18.6k tokens)", dim)],
    ]

    with tempfile.TemporaryDirectory(prefix="desk-gif-") as tmp:
        tdir = pathlib.Path(tmp)
        for idx, lines in enumerate(steps):
            buf = bytearray(w * h * 3)
            for y in range(h):
                row_col = bar if y < 28 else bg
                buf[y*w*3:(y+1)*w*3] = bytes(row_col) * w
            draw_text(buf, 18, 8, "DESKCODE TERMINAL", dim, scale=2)
            yy = 44
            for text, col in lines:
                draw_text(buf, 20, yy, text, col, scale=2)
                yy += 28
            write_png(tdir / f"frame_{idx:02d}.png", buf)
        subprocess.run(
            [ffmpeg, "-y", "-loglevel", "error", "-framerate", "0.7",
             "-i", str(tdir / "frame_%02d.png"),
             "-vf", "split[s0][s1];[s0]palettegen=max_colors=32[p];[s1][p]paletteuse",
             str(out_gif)],
            check=True,
        )


def main() -> None:
    ASSETS.mkdir(parents=True, exist_ok=True)
    (ASSETS / "deskcode-terminal.svg").write_text(TERMINAL_SVG, encoding="utf-8")
    (ASSETS / "deskcode-gui.svg").write_text(GUI_SVG, encoding="utf-8")
    make_gif_with_ffmpeg(ASSETS / "deskcode-demo.gif")
    for name in ("deskcode-terminal.svg", "deskcode-gui.svg", "deskcode-demo.gif"):
        p = ASSETS / name
        if p.exists():
            print(f"{name}: {p.stat().st_size} bytes")


if __name__ == "__main__":
    main()
