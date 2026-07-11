import argparse
import json
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse


STATE_LABELS_ZH = {
    "active": "工作中",
    "notLoaded": "未运行",
    "complete": "已完成",
    "blocked": "已阻塞",
}

UNTITLED_SESSION_ZH = "未命名会话"
UNKNOWN_STATUS_ZH = "未知"


def _load_sessions_file(sessions_file):
    if not sessions_file:
        return None

    path = Path(sessions_file)
    if not path.is_file():
        return None

    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


def _string_value(value, default=""):
    if value is None:
        return default
    return str(value)


def _normalize_session(session, payload_updated_at):
    if not isinstance(session, dict):
        session = {}

    state = _string_value(session.get("state"), "notLoaded")
    return {
        "id": _string_value(session.get("id")),
        "title": _string_value(session.get("title"), UNTITLED_SESSION_ZH),
        "state": state,
        "status_zh": _string_value(
            session.get("status_zh"), STATE_LABELS_ZH.get(state, UNKNOWN_STATUS_ZH)
        ),
        "cwd": _string_value(session.get("cwd")),
        "updated_at": _string_value(session.get("updated_at"), payload_updated_at),
        "completed_at": _string_value(session.get("completed_at")),
        "last_user_at": _string_value(session.get("last_user_at")),
    }


def build_status_payload(sessions_file=None):
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    source = _load_sessions_file(sessions_file)
    if not isinstance(source, dict):
        return {"updated_at": now, "sessions": []}

    updated_at = _string_value(source.get("updated_at"), now)
    sessions = source.get("sessions")
    if not isinstance(sessions, list):
        sessions = []

    return {
        "updated_at": updated_at,
        "sessions": [_normalize_session(session, updated_at) for session in sessions],
    }


class _StatusHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path != "/status":
            self.send_response(404)
            self.end_headers()
            return

        body = json.dumps(
            build_status_payload(sessions_file=self.server.sessions_file), ensure_ascii=False
        ).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        return


class StatusServer(ThreadingHTTPServer):
    def __init__(self, host="0.0.0.0", port=8787, sessions_file=None):
        super().__init__((host, port), _StatusHandler)
        self.port = self.server_address[1]
        self.sessions_file = sessions_file


def main():
    parser = argparse.ArgumentParser(description="Serve Codex status JSON for ESP32 displays.")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", default=8787, type=int)
    parser.add_argument("--sessions-file", help="Read exported Codex sessions from this JSON file.")
    args = parser.parse_args()

    server = StatusServer(
        host=args.host,
        port=args.port,
        sessions_file=args.sessions_file,
    )
    print(f"Serving Codex status at http://{args.host}:{server.port}/status")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
