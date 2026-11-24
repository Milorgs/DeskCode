"""Zero-dependency HTTP + SSE client for ``desk serve`` shared sessions."""
from __future__ import annotations

import json
import urllib.error
import urllib.request
from typing import Any, Callable, Dict, Iterator, List, Optional, Tuple

from ._errors import DeskError


class DeskServeClient:
    """Connects to a running ``desk serve`` daemon over HTTP + SSE."""

    def __init__(self, base_url: str = "http://127.0.0.1:7890", token: str = "", timeout: float = 30.0) -> None:
        self.base_url = base_url.rstrip("/")
        self.token = token
        self.timeout = timeout

    def _headers(self, accept: str = "application/json") -> Dict[str, str]:
        hdrs = {"Accept": accept, "Content-Type": "application/json"}
        if self.token:
            hdrs["Authorization"] = f"Bearer {self.token}"
        return hdrs

    def _request(self, method: str, path: str, body: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        data = None if body is None else json.dumps(body).encode("utf-8")
        req = urllib.request.Request(
            f"{self.base_url}{path}",
            data=data,
            headers=self._headers(),
            method=method,
        )
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                raw = resp.read().decode("utf-8")
                return json.loads(raw) if raw else {}
        except urllib.error.HTTPError as exc:
            try:
                detail = exc.read().decode("utf-8", errors="replace")
            finally:
                exc.close()
            raise DeskError(f"desk serve HTTP {exc.code}: {detail}") from exc
        except urllib.error.URLError as exc:
            raise DeskError(f"desk serve connection error: {exc.reason}") from exc

    def health(self) -> Dict[str, Any]:
        return self._request("GET", "/api/health")

    def list_sessions(self) -> List[Dict[str, Any]]:
        return list(self._request("GET", "/api/sessions").get("sessions", []))

    def create_session(self, cwd: Optional[str] = None, mode: str = "ask", no_net: bool = False) -> Dict[str, Any]:
        payload: Dict[str, Any] = {"mode": mode, "no_net": no_net}
        if cwd:
            payload["cwd"] = cwd
        return dict(self._request("POST", "/api/sessions", payload).get("session", {}))

    def get_session(self, session_id: str) -> Dict[str, Any]:
        return self._request("GET", f"/api/sessions/{session_id}")

    def set_mode(self, session_id: str, mode: str, no_net: Optional[bool] = None) -> Dict[str, Any]:
        payload: Dict[str, Any] = {"mode": mode}
        if no_net is not None:
            payload["no_net"] = no_net
        return self._request("POST", f"/api/sessions/{session_id}/mode", payload)

    def send_prompt(self, session_id: str, prompt: str, mode: Optional[str] = None) -> Dict[str, Any]:
        payload: Dict[str, Any] = {"prompt": prompt}
        if mode:
            payload["mode"] = mode
        return self._request("POST", f"/api/sessions/{session_id}/prompt", payload)

    def approve(self, session_id: str, allow: bool = True, request_id: str = "") -> Dict[str, Any]:
        return self._request(
            "POST",
            f"/api/sessions/{session_id}/approve",
            {"allow": allow, "request_id": request_id},
        )

    def cancel(self, session_id: str) -> Dict[str, Any]:
        return self._request("POST", f"/api/sessions/{session_id}/cancel", {})

    def stream_events(
        self,
        session_id: str,
        on_ready: Optional[Callable[[], None]] = None,
    ) -> Iterator[Tuple[str, Dict[str, Any]]]:
        """Yield ``(event_name, payload_dict)`` frames from the session SSE stream."""
        req = urllib.request.Request(
            f"{self.base_url}/api/sessions/{session_id}/events",
            headers=self._headers(accept="text/event-stream"),
            method="GET",
        )
        with urllib.request.urlopen(req, timeout=self.timeout) as resp:
            if on_ready is not None:
                on_ready()
            event_name = ""
            data_lines: List[str] = []
            for raw_line in resp:
                line = raw_line.decode("utf-8", errors="replace").rstrip("\r\n")
                if not line:
                    if event_name and data_lines:
                        yield event_name, json.loads("\n".join(data_lines))
                    event_name = ""
                    data_lines = []
                elif line.startswith("event:"):
                    event_name = line[6:].strip()
                elif line.startswith("data:"):
                    val = line[5:]
                    if val.startswith(" "):
                        val = val[1:]
                    data_lines.append(val)
