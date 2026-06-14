import argparse
import json
import os
import time
from datetime import datetime, timezone
from pathlib import Path


STATE_LABELS_ZH = {
    "active": "\u5de5\u4f5c\u4e2d",
    "notLoaded": "\u672a\u52a0\u8f7d",
}
MAX_SCREEN_TITLE_CHARS = 24
MAX_DETAIL_MESSAGES = 4
MAX_DETAIL_CHARS = 260


def _default_codex_home():
    return Path(os.environ.get("CODEX_HOME", Path.home() / ".codex"))


def _read_json_file(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def _read_session_index(codex_home):
    index_file = Path(codex_home) / "session_index.jsonl"
    sessions = []
    try:
        lines = index_file.read_text(encoding="utf-8").splitlines()
    except OSError:
        return sessions

    for line in lines:
        if not line.strip():
            continue
        try:
            entry = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(entry, dict):
            sessions.append(entry)
    return sessions


def _message_text(payload):
    if not isinstance(payload, dict):
        return ""
    if payload.get("type") != "message":
        return ""
    parts = []
    content = payload.get("content")
    if not isinstance(content, list):
        return ""
    for item in content:
        if isinstance(item, dict) and item.get("type") in {"input_text", "output_text", "text"}:
            parts.append(str(item.get("text") or ""))
    text = " ".join(part.strip() for part in parts if part.strip()).strip()
    if text.startswith("<environment_context>"):
        return ""
    return text


def _text_from_message_payload(payload):
    if not isinstance(payload, dict) or payload.get("role") != "user":
        return ""
    return _message_text(payload)


def _detail_line_from_message_payload(payload):
    if not isinstance(payload, dict):
        return ""
    role = payload.get("role")
    if role not in {"user", "assistant"}:
        return ""
    text = _message_text(payload)
    if not text:
        return ""
    prefix = "\u7528\u6237\uff1a" if role == "user" else "\u52a9\u624b\uff1a"
    return f"{prefix}{text}"


def _screen_detail(lines):
    detail = "\n".join(lines[-MAX_DETAIL_MESSAGES:])
    if len(detail) <= MAX_DETAIL_CHARS:
        return detail
    return detail[: MAX_DETAIL_CHARS - 3].rstrip() + "..."


def _read_rollout_sessions(codex_home, scan_limit=20):
    sessions_dir = Path(codex_home) / "sessions"
    try:
        rollout_files = sorted(
            sessions_dir.rglob("rollout-*.jsonl"),
            key=lambda path: path.stat().st_mtime,
            reverse=True,
        )[:scan_limit]
    except OSError:
        return []

    sessions = []
    for rollout_file in rollout_files:
        entry = {}
        try:
            lines = rollout_file.read_text(encoding="utf-8").splitlines()
        except OSError:
            continue

        for line in lines:
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            if event.get("type") == "session_meta" and isinstance(event.get("payload"), dict):
                payload = event["payload"]
                entry["id"] = str(payload.get("id") or "")
                entry["cwd"] = str(payload.get("cwd") or "")
                entry["updated_at"] = str(event.get("timestamp") or payload.get("timestamp") or "")
            elif event.get("type") == "response_item" and not entry.get("thread_name"):
                title = _text_from_message_payload(event.get("payload"))
                if title:
                    entry["thread_name"] = title
            if event.get("type") == "response_item":
                detail_line = _detail_line_from_message_payload(event.get("payload"))
                if detail_line:
                    entry.setdefault("detail_lines", []).append(detail_line)

        if entry.get("id"):
            entry.setdefault("thread_name", "\u672a\u547d\u540d\u4f1a\u8bdd")
            entry["detail"] = _screen_detail(entry.get("detail_lines", []))
            entry.pop("detail_lines", None)
            sessions.append(entry)
    return sessions


def _parse_time(value):
    if not value:
        return datetime.min.replace(tzinfo=timezone.utc)
    text = str(value)
    if "." in text:
        prefix, suffix = text.split(".", 1)
        digits = []
        rest = ""
        for index, char in enumerate(suffix):
            if char.isdigit():
                digits.append(char)
            else:
                rest = suffix[index:]
                break
        else:
            rest = ""
        text = f"{prefix}.{''.join(digits)[:6]}{rest}"
    try:
        return datetime.fromisoformat(text.replace("Z", "+00:00"))
    except ValueError:
        return datetime.min.replace(tzinfo=timezone.utc)


def _format_time(value):
    parsed = _parse_time(value)
    if parsed == datetime.min.replace(tzinfo=timezone.utc):
        return ""
    return parsed.astimezone().strftime("%Y-%m-%d %H:%M:%S")


def _workspace_name(path):
    if not path:
        return ""
    return Path(str(path)).name


def _screen_title(value):
    title = str(value or "\u672a\u547d\u540d\u4f1a\u8bdd").strip()
    title = " ".join(title.split())
    if len(title) <= MAX_SCREEN_TITLE_CHARS:
        return title
    return title[: MAX_SCREEN_TITLE_CHARS - 3].rstrip() + "..."


def _thread_workspace(thread_id, state):
    hints = state.get("thread-workspace-root-hints")
    if isinstance(hints, dict):
        return hints.get(thread_id, "")
    return ""


def _entry_workspace(entry, state):
    workspace = _thread_workspace(str(entry.get("id", "")), state)
    return workspace or str(entry.get("cwd") or "")


def _is_active_thread(thread_id, state):
    thread_workspace = _thread_workspace(thread_id, state)
    active_roots = state.get("active-workspace-roots")
    if not thread_workspace or not isinstance(active_roots, list):
        return False
    normalized_thread_workspace = os.path.normcase(os.path.abspath(thread_workspace))
    return any(
        os.path.normcase(os.path.abspath(str(root))) == normalized_thread_workspace
        for root in active_roots
    )


def _is_active_entry(entry, state):
    workspace = _entry_workspace(entry, state)
    active_roots = state.get("active-workspace-roots")
    if not workspace or not isinstance(active_roots, list):
        return False
    normalized_workspace = os.path.normcase(os.path.abspath(workspace))
    return any(
        os.path.normcase(os.path.abspath(str(root))) == normalized_workspace for root in active_roots
    )


def build_sessions_payload(codex_home=None, limit=5):
    codex_home = Path(codex_home) if codex_home else _default_codex_home()
    state = _read_json_file(codex_home / ".codex-global-state.json")
    entries_by_id = {}
    for entry in _read_session_index(codex_home) + _read_rollout_sessions(codex_home):
        thread_id = str(entry.get("id", ""))
        if not thread_id:
            continue
        existing = entries_by_id.get(thread_id)
        if existing is None or _parse_time(entry.get("updated_at")) >= _parse_time(existing.get("updated_at")):
            entries_by_id[thread_id] = entry

    entries = sorted(
        entries_by_id.values(),
        key=lambda entry: _parse_time(entry.get("updated_at")),
        reverse=True,
    )[:limit]

    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    sessions = []
    for entry in entries:
        status_state = "active" if _is_active_entry(entry, state) else "notLoaded"
        workspace = _entry_workspace(entry, state)
        sessions.append(
            {
                "title": _screen_title(entry.get("thread_name")),
                "state": status_state,
                "status_zh": STATE_LABELS_ZH[status_state],
                "cwd": _workspace_name(workspace),
                "updated_at": _format_time(entry.get("updated_at")) or now,
                "detail": str(entry.get("detail") or ""),
            }
        )

    return {"updated_at": now, "sessions": sessions}


def export_sessions(codex_home=None, output_file="sessions.json", limit=5):
    output_path = Path(output_file)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    payload = build_sessions_payload(codex_home=codex_home, limit=limit)
    output_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    return payload


def main():
    parser = argparse.ArgumentParser(description="Export Codex session index for the ESP32 status bridge.")
    parser.add_argument("--codex-home", default=str(_default_codex_home()))
    parser.add_argument("--output", default="sessions.json")
    parser.add_argument("--limit", default=5, type=int)
    parser.add_argument("--watch", action="store_true", help="Continuously refresh the output file.")
    parser.add_argument("--interval", default=1.0, type=float, help="Refresh interval in seconds for --watch.")
    args = parser.parse_args()

    while True:
        payload = export_sessions(codex_home=args.codex_home, output_file=args.output, limit=args.limit)
        print(f"Exported {len(payload['sessions'])} sessions to {args.output}")
        if not args.watch:
            break
        time.sleep(args.interval)


if __name__ == "__main__":
    main()
