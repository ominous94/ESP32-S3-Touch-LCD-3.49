import argparse
import json
import os
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path


STATE_LABELS_ZH = {
    "active": "\u5de5\u4f5c\u4e2d",
    "notLoaded": "\u672a\u8fd0\u884c",
    "complete": "\u5df2\u5b8c\u6210",
    "blocked": "\u5df2\u963b\u585e",
}
MAX_SCREEN_TITLE_CHARS = 24
ACTIVE_RECENT_SECONDS = 120
COMPLETE_VISIBLE_SECONDS = 600

# Number of lines to read from the tail of each rollout file.  Rollout files are
# JSONL appended sequentially; the session_meta is at the top, but the event
# types we care about (last_event_msg_type, completed_at) are near the bottom.
# Reading only the tail avoids loading multi-megabyte files in full every cycle.
_ROLLOUT_TAIL_LINES = 200


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


def _tail_lines(path, n=_ROLLOUT_TAIL_LINES):
    """Read the last *n* lines of *path* efficiently.

    For files smaller than a threshold we fall back to ``read_text().splitlines()``.
    For large files we seek from the end and decode backwards to avoid loading
    the entire file into memory.
    """
    try:
        size = path.stat().st_size
    except OSError:
        return []

    # Small file: just read it all.
    if size <= 65536:
        try:
            return path.read_text(encoding="utf-8").splitlines()
        except OSError:
            return []

    # Large file: read from the end.
    # Estimate ~4 bytes per line for safety; read at least 8 KB.
    chunk_size = max(8192, n * 8)
    try:
        with open(path, "rb") as fh:
            fh.seek(0, os.SEEK_END)
            pos = fh.tell()
            collected = b""
            while pos > 0 and collected.count(b"\n") <= n:
                read_size = min(chunk_size, pos)
                pos -= read_size
                fh.seek(pos)
                chunk = fh.read(read_size)
                collected = chunk + collected
                if len(collected) > chunk_size * 4:  # safety cap
                    break
            # Trim to last n lines.
            all_lines = collected.decode("utf-8", errors="replace").splitlines()
            return all_lines[-n:] if len(all_lines) > n else all_lines
    except OSError:
        return []


def _read_rollout_sessions(codex_home, scan_limit=20, mtime_cache=None):
    """Read recent rollout session files.

    Parameters
    ----------
    mtime_cache : dict | None
        If provided, used to skip files whose mtime hasn't changed since the
        last call.  The dict maps ``str(path)`` → ``(mtime, entry_dict)`` and is
        updated in place with fresh entries.
    """
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
    cache = mtime_cache if mtime_cache is not None else {}

    for rollout_file in rollout_files:
        try:
            stat = rollout_file.stat()
            mtime = stat.st_mtime
        except OSError:
            continue

        cache_key = str(rollout_file)
        cached = cache.get(cache_key)

        if cached is not None and cached[0] == mtime:
            # File hasn't changed — reuse cached entry.
            entry = cached[1]
            if entry:
                sessions.append(entry)
            continue

        # File is new or modified — read it.
        entry = _parse_rollout_file(rollout_file, mtime)
        cache[cache_key] = (mtime, entry)
        if entry:
            sessions.append(entry)

    # Prune cache entries that are no longer in the top scan_limit.
    current_keys = {str(f) for f in rollout_files}
    stale_keys = [k for k in cache if k not in current_keys]
    for k in stale_keys:
        del cache[k]

    return sessions


def _parse_rollout_file(rollout_file, mtime):
    """Parse a single rollout JSONL file, returning an entry dict."""
    lines = _tail_lines(rollout_file)
    if not lines:
        return {}

    entry = {}
    last_event_msg_type = ""

    # We need session_meta which is at the top of the file.  If our tail read
    # didn't capture it, we'll try reading the first few lines.
    has_meta = any('"session_meta"' in line for line in lines[:5])

    if not has_meta:
        try:
            with open(rollout_file, "r", encoding="utf-8") as fh:
                # Read just enough lines to find session_meta.
                for _ in range(10):
                    line = fh.readline()
                    if not line:
                        break
                    try:
                        event = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if event.get("type") == "session_meta" and isinstance(event.get("payload"), dict):
                        payload = event["payload"]
                        entry["id"] = str(payload.get("id") or "")
                        entry["cwd"] = str(payload.get("cwd") or "")
                        entry["updated_at"] = str(event.get("timestamp") or payload.get("timestamp") or "")
                        break
        except OSError:
            pass
    else:
        # session_meta is in the tail we already read.
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
                break

    # Scan all tail lines for thread_name and event status.
    for line in lines:
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("type") == "response_item" and not entry.get("thread_name"):
            title = _text_from_message_payload(event.get("payload"))
            if title:
                entry["thread_name"] = title
        if event.get("type") == "response_item":
            if isinstance(event.get("payload"), dict) and event["payload"].get("role") == "user":
                user_ts = str(event.get("timestamp") or "")
                if user_ts:
                    entry["last_user_at"] = user_ts
        if event.get("type") == "event_msg" and isinstance(event.get("payload"), dict):
            msg_type = str(event["payload"].get("type") or "")
            if msg_type:
                last_event_msg_type = msg_type
                if msg_type in {"task_complete", "shutdown_complete"}:
                    entry["completed_at"] = str(event.get("timestamp") or "")

    if entry.get("id"):
        entry.setdefault("thread_name", "\u672a\u547d\u540d\u4f1a\u8bdd")
        entry["last_event_msg_type"] = last_event_msg_type
        entry["rollout_mtime"] = mtime
    return entry


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
    last_event = str(entry.get("last_event_msg_type") or "")
    if last_event in {"task_complete", "error", "shutdown_complete"}:
        return False
    rollout_mtime = float(entry.get("rollout_mtime") or 0.0)
    if rollout_mtime <= 0:
        return False
    if (time.time() - rollout_mtime) > ACTIVE_RECENT_SECONDS:
        return False
    if not last_event:
        return False
    return True


def _completion_time(entry):
    completed_at = _parse_time(entry.get("completed_at"))
    if completed_at != datetime.min.replace(tzinfo=timezone.utc):
        return completed_at
    rollout_mtime = float(entry.get("rollout_mtime") or 0.0)
    if rollout_mtime > 0:
        return datetime.fromtimestamp(rollout_mtime, tz=timezone.utc)
    return _parse_time(entry.get("updated_at"))


def _entry_state(entry, state, now=None):
    now = now or datetime.now(timezone.utc)
    last_event = str(entry.get("last_event_msg_type") or "")
    if last_event in {"task_complete", "shutdown_complete"}:
        completed_at = _completion_time(entry)
        if completed_at != datetime.min.replace(tzinfo=timezone.utc):
            if completed_at.tzinfo is None:
                completed_at = completed_at.replace(tzinfo=timezone.utc)
            if (now - completed_at).total_seconds() > COMPLETE_VISIBLE_SECONDS:
                return "notLoaded"
        return "complete"
    if last_event == "error":
        return "blocked"
    if _is_active_entry(entry, state):
        return "active"
    return "notLoaded"


def _state_priority(state):
    priorities = {
        "active": 0,
        "blocked": 1,
        "complete": 2,
        "notLoaded": 3,
    }
    return priorities.get(state, 4)


def _entry_sort_time(entry):
    completion = _completion_time(entry)
    if completion != datetime.min.replace(tzinfo=timezone.utc):
        return completion
    return _parse_time(entry.get("updated_at"))


# Module-level mtime cache for the --watch loop.  Keyed by file path, maps to
# (mtime, entry_dict).  Persists across calls so unchanged files are skipped.
_mtime_cache = {}


def build_sessions_payload(codex_home=None, limit=5, now=None):
    codex_home = Path(codex_home) if codex_home else _default_codex_home()
    state = _read_json_file(codex_home / ".codex-global-state.json")
    now_dt = now or datetime.now(timezone.utc)
    entries_by_id = {}
    for entry in _read_session_index(codex_home) + _read_rollout_sessions(
        codex_home, mtime_cache=_mtime_cache
    ):
        thread_id = str(entry.get("id", ""))
        if not thread_id:
            continue
        existing = entries_by_id.get(thread_id)
        if existing is None or _parse_time(entry.get("updated_at")) >= _parse_time(existing.get("updated_at")):
            merged = dict(entry)
            if existing:
                for key in ("last_event_msg_type", "rollout_mtime", "completed_at", "last_user_at"):
                    if key not in merged and key in existing:
                        merged[key] = existing[key]
            entries_by_id[thread_id] = merged
        else:
            for key in ("last_event_msg_type", "rollout_mtime", "completed_at", "last_user_at"):
                if key in entry and key not in existing:
                    existing[key] = entry[key]

    entries = sorted(
        entries_by_id.values(),
        key=_entry_sort_time,
        reverse=True,
    )
    entries = sorted(
        entries,
        key=lambda entry: _state_priority(_entry_state(entry, state, now_dt)),
    )[:limit]

    now_text = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    sessions = []
    for entry in entries:
        status_state = _entry_state(entry, state, now_dt)
        workspace = _entry_workspace(entry, state)
        sessions.append(
            {
                "id": str(entry.get("id") or ""),
                "title": _screen_title(entry.get("thread_name")),
                "state": status_state,
                "status_zh": STATE_LABELS_ZH[status_state],
                "cwd": _workspace_name(workspace),
                "updated_at": _format_time(entry.get("updated_at")) or now_text,
                "completed_at": _format_time(entry.get("completed_at")),
                "last_user_at": _format_time(entry.get("last_user_at")),
            }
        )

    return {"updated_at": now_text, "sessions": sessions}


def _atomic_write(path, content):
    """Write *content* to *path* atomically by using a temp file + os.replace."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    # Use a temp file in the same directory to ensure same-filesystem rename.
    fd, tmp_path = tempfile.mkstemp(
        dir=str(path.parent), prefix=".tmp_", suffix=path.suffix
    )
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(content)
        os.replace(tmp_path, str(path))
    except Exception:
        try:
            os.unlink(tmp_path)
        except OSError:
            pass
        raise


def export_sessions(codex_home=None, output_file="sessions.json", limit=5):
    output_path = Path(output_file)
    payload = build_sessions_payload(codex_home=codex_home, limit=limit)
    _atomic_write(output_path, json.dumps(payload, ensure_ascii=False, indent=2))
    return payload


def main():
    parser = argparse.ArgumentParser(description="Export Codex session index for the ESP32 status bridge.")
    parser.add_argument("--codex-home", default=str(_default_codex_home()))
    parser.add_argument("--output", default="sessions.json")
    parser.add_argument("--limit", default=5, type=int)
    parser.add_argument("--watch", action="store_true", help="Continuously refresh the output file.")
    parser.add_argument("--interval", default=5.0, type=float, help="Refresh interval in seconds for --watch.")
    parser.add_argument(
        "--log-every",
        default=60,
        type=int,
        help="Print a log line every N export cycles (default: 60). Set to 1 to log every cycle.",
    )
    args = parser.parse_args()

    cycle = 0
    while True:
        payload = export_sessions(
            codex_home=args.codex_home,
            output_file=args.output,
            limit=args.limit,
        )
        cycle += 1
        if args.log_every <= 1 or cycle % args.log_every == 0:
            print(f"Exported {len(payload['sessions'])} sessions to {args.output}", flush=True)
        if not args.watch:
            break
        time.sleep(args.interval)


if __name__ == "__main__":
    main()
