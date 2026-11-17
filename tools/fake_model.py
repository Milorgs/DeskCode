"""本地假模型服务器，供界面测试使用；不联网、不需要真实密钥。

兼容 OpenAI Chat Completions 的流式（SSE）与非流式请求：
- 用户消息里带 "echo" 且本轮还没有工具结果时，返回一次 run_shell 调用，用来测试审批面板；
- 其他情况流式返回一段固定文字，用来测试流式显示、用时与词元统计。

用法：python3 -I tools/fake_model.py <端口文件>
启动后把实际监听的端口写进端口文件。
"""
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def reply_for(messages):
    last_user = next((m for m in reversed(messages) if m.get("role") == "user"), {})
    text = last_user.get("content") or ""
    if isinstance(text, list):
        text = " ".join(part.get("text", "") for part in text if isinstance(part, dict))
    answered = messages and messages[-1].get("role") == "tool"
    if "echo" in text and not answered:
        return None, {"id": "call_1", "type": "function",
                      "function": {"name": "run_shell", "arguments": json.dumps({"command": "echo hi"})}}
    if "edit" in text and not answered:
        # Ask to create a file so the approval panel shows its diff preview.
        content = "# 示例\n\ndef greet(name):\n    return f\"你好，{name}\"\n"
        return None, {"id": "call_2", "type": "function",
                      "function": {"name": "write_file", "arguments": json.dumps(
                          {"path": "demo.py", "content": content, "expected_sha256": None}, ensure_ascii=False)}}
    if answered:
        return "命令已执行，输出是 hi。", None
    if "markdown" in text:
        return ("## 项目结构\n\n"
                "这个项目是一个 **C++20** 终端助手，入口在 `src/main.cpp`。\n\n"
                "- `src/agent.cpp`：模型与工具循环\n"
                "- `src/files.cpp`：文件读写、搜索与编辑\n"
                "- `src/ui.cpp`：终端排版\n\n"
                "```sh\nmake -j8 && make test\n```\n\n"
                "> 修改界面后请在真实终端里截图确认。\n"), None
    return "这是本地假模型的回复：收到「" + text[:40] + "」。用于测试流式显示。", None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        body = json.dumps({"data": [{"id": "fake-model"}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        request = json.loads(self.rfile.read(length) or b"{}")
        text, call = reply_for(request.get("messages") or [])
        usage = {"prompt_tokens": 1200, "completion_tokens": 80, "total_tokens": 1280}
        if not request.get("stream"):
            message = {"role": "assistant", "content": text}
            if call:
                message["tool_calls"] = [call]
            body = json.dumps({"choices": [{"message": message, "finish_reason": "tool_calls" if call else "stop"}],
                               "usage": usage}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()

        def send(obj):
            self.wfile.write(b"data: " + json.dumps(obj, ensure_ascii=False).encode() + b"\n\n")
            self.wfile.flush()

        time.sleep(0.8)
        if call:
            send({"choices": [{"index": 0, "delta": {"tool_calls": [dict(call, index=0)]}}]})
            send({"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}], "usage": usage})
        else:
            for i in range(0, len(text), 3):
                send({"choices": [{"index": 0, "delta": {"content": text[i:i + 3]}}]})
                time.sleep(0.05)
            send({"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}], "usage": usage})
        self.wfile.write(b"data: [DONE]\n\n")


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    with open(sys.argv[1], "w") as handle:
        handle.write(str(server.server_address[1]))
    server.serve_forever()


if __name__ == "__main__":
    main()
