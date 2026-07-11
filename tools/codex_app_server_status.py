import copy
import json
import subprocess
import threading
from datetime import datetime, timezone


ALL_THREAD_SOURCE_KINDS = [
    "cli",
    "vscode",
    "exec",
    "appServer",
    "subAgent",
    "subAgentReview",
    "subAgentCompact",
    "subAgentThreadSpawn",
    "subAgentOther",
    "unknown",
]


class CodexAppServerError(RuntimeError):
    pass


class CodexAppServerStatus:
    """Small JSONL client for the stable Codex app-server status surface."""

    def __init__(self, command=None, request_timeout=8.0):
        self.command = list(command or ["codex", "app-server"])
        self.request_timeout = float(request_timeout)
        self._process = None
        self._reader_thread = None
        self._stderr_thread = None
        self._write_lock = threading.Lock()
        self._state_lock = threading.Lock()
        self._pending_lock = threading.Lock()
        self._pending = {}
        self._next_request_id = 1
        self._threads = {}
        self._turn_status = {}
        self._last_error = ""

    @property
    def last_error(self):
        with self._state_lock:
            return self._last_error

    def start(self):
        if self._process is not None and self._process.poll() is None:
            return
        self.close()
        try:
            self._process = subprocess.Popen(
                self.command,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
            )
        except OSError as exc:
            raise CodexAppServerError(f"无法启动 Codex App Server：{exc}") from exc

        self._reader_thread = threading.Thread(
            target=self._read_stdout, name="codex-app-server-reader", daemon=True
        )
        self._stderr_thread = threading.Thread(
            target=self._read_stderr, name="codex-app-server-stderr", daemon=True
        )
        self._reader_thread.start()
        self._stderr_thread.start()

        self._request(
            "initialize",
            {
                "clientInfo": {
                    "name": "esp32_codex_status",
                    "title": "ESP32 Codex Status",
                    "version": "1.0.0",
                },
                "capabilities": {
                    "optOutNotificationMethods": [
                        "item/agentMessage/delta",
                        "item/reasoning/summaryTextDelta",
                        "item/commandExecution/outputDelta",
                        "turn/diff/updated",
                        "turn/plan/updated",
                        "thread/tokenUsage/updated",
                    ]
                },
            },
            request_id=0,
        )
        self._notify("initialized", {})

    def close(self):
        process = self._process
        self._process = None
        if process is not None:
            try:
                if process.stdin:
                    process.stdin.close()
            except OSError:
                pass
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    process.kill()
        self._fail_pending("Codex App Server 已关闭")
        current = threading.current_thread()
        for thread in (self._reader_thread, self._stderr_thread):
            if thread is not None and thread is not current and thread.is_alive():
                thread.join(timeout=1.0)
        self._reader_thread = None
        self._stderr_thread = None

    def refresh(self, limit=20):
        self.start()
        result = self._request(
            "thread/list",
            {
                "limit": max(int(limit), 1),
                "sortKey": "updated_at",
                "sortDirection": "desc",
                "modelProviders": [],
                "sourceKinds": ALL_THREAD_SOURCE_KINDS,
                # Avoid a potentially unbounded rollout repair scan. The legacy
                # fallback below is responsible for sessions missing from the DB.
                "useStateDbOnly": True,
            },
        )
        data = result.get("data") if isinstance(result, dict) else None
        if not isinstance(data, list):
            raise CodexAppServerError("thread/list 返回了无效数据")
        with self._state_lock:
            for thread in data:
                if isinstance(thread, dict) and thread.get("id"):
                    self._threads[str(thread["id"])] = copy.deepcopy(thread)
            self._last_error = ""
        return self.snapshot()

    def snapshot(self):
        with self._state_lock:
            threads = copy.deepcopy(list(self._threads.values()))
            turn_status = copy.deepcopy(self._turn_status)
        for thread in threads:
            thread_id = str(thread.get("id") or "")
            if thread_id in turn_status:
                thread["lastTurnStatus"] = turn_status[thread_id]
        return threads

    def handle_notification(self, method, params):
        """Apply a server notification. Public to make protocol behavior testable."""
        params = params if isinstance(params, dict) else {}
        thread_id = str(params.get("threadId") or "")
        with self._state_lock:
            if method == "thread/status/changed" and thread_id:
                thread = self._threads.setdefault(thread_id, {"id": thread_id})
                status = params.get("status")
                if isinstance(status, dict):
                    thread["status"] = copy.deepcopy(status)
                thread["updatedAt"] = int(datetime.now(timezone.utc).timestamp())
            elif method in {"turn/started", "turn/completed"} and thread_id:
                turn = params.get("turn")
                if isinstance(turn, dict):
                    status = str(turn.get("status") or "")
                    if status:
                        self._turn_status[thread_id] = status

    def _request(self, method, params, request_id=None):
        if request_id is None:
            with self._pending_lock:
                request_id = self._next_request_id
                self._next_request_id += 1
        waiter = {"event": threading.Event(), "message": None}
        with self._pending_lock:
            self._pending[request_id] = waiter
        try:
            self._write({"method": method, "id": request_id, "params": params})
            if not waiter["event"].wait(self.request_timeout):
                raise CodexAppServerError(f"App Server 请求超时：{method}")
            message = waiter["message"] or {}
            if "error" in message:
                error = message.get("error") or {}
                raise CodexAppServerError(
                    f"App Server {method} 失败：{error.get('message', error)}"
                )
            result = message.get("result")
            return result if isinstance(result, dict) else {}
        finally:
            with self._pending_lock:
                self._pending.pop(request_id, None)

    def _notify(self, method, params):
        self._write({"method": method, "params": params})

    def _write(self, message):
        process = self._process
        if process is None or process.poll() is not None or process.stdin is None:
            raise CodexAppServerError("Codex App Server 未运行")
        line = json.dumps(message, ensure_ascii=False, separators=(",", ":")) + "\n"
        try:
            with self._write_lock:
                process.stdin.write(line)
                process.stdin.flush()
        except OSError as exc:
            raise CodexAppServerError(f"写入 Codex App Server 失败：{exc}") from exc

    def _read_stdout(self):
        process = self._process
        if process is None or process.stdout is None:
            return
        try:
            for line in process.stdout:
                try:
                    message = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if "id" in message:
                    with self._pending_lock:
                        waiter = self._pending.get(message.get("id"))
                    if waiter is not None:
                        waiter["message"] = message
                        waiter["event"].set()
                elif message.get("method"):
                    self.handle_notification(message["method"], message.get("params"))
        finally:
            self._fail_pending("Codex App Server 连接已断开")

    def _read_stderr(self):
        process = self._process
        if process is None or process.stderr is None:
            return
        for line in process.stderr:
            text = line.strip()
            if text:
                with self._state_lock:
                    self._last_error = text[-500:]

    def _fail_pending(self, message):
        with self._state_lock:
            self._last_error = message
        with self._pending_lock:
            waiters = list(self._pending.values())
        for waiter in waiters:
            waiter["message"] = {"error": {"message": message}}
            waiter["event"].set()


def app_server_thread_to_entry(thread):
    thread = thread if isinstance(thread, dict) else {}
    status = thread.get("status")
    status = status if isinstance(status, dict) else {}
    status_type = str(status.get("type") or "notLoaded")
    active_flags = status.get("activeFlags")
    active_flags = active_flags if isinstance(active_flags, list) else []
    turn_status = str(thread.get("lastTurnStatus") or "")

    if turn_status == "failed" or status_type == "systemError":
        state = "blocked"
    elif status_type == "active" or turn_status == "inProgress":
        state = "active"
    elif status_type == "idle" or turn_status == "completed":
        state = "complete"
    else:
        state = "notLoaded"

    if "waitingOnApproval" in active_flags:
        status_zh = "等待处理"
    else:
        status_zh = {
            "active": "工作中",
            "complete": "已完成",
            "blocked": "任务失败",
            "notLoaded": "未运行",
        }[state]

    updated_at = thread.get("updatedAt")
    try:
        updated_at = datetime.fromtimestamp(
            int(updated_at), tz=timezone.utc
        ).isoformat().replace("+00:00", "Z")
    except (TypeError, ValueError, OSError):
        updated_at = ""

    title = thread.get("name") or thread.get("preview") or "未命名会话"
    return {
        "id": str(thread.get("id") or ""),
        "thread_name": str(title),
        "cwd": str(thread.get("cwd") or ""),
        "updated_at": updated_at,
        "last_user_at": updated_at,
        "app_server_state": state,
        "app_server_status_zh": status_zh,
        "app_server_status_type": status_type,
        "app_server_active_flags": active_flags,
        "status_source": "appServer",
    }
