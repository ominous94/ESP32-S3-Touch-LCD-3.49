import socket
import sys
import threading
from pathlib import Path

from tools.codex_status_bridge import StatusServer
from tools.export_codex_sessions import export_sessions


class CodexStatusService:
    def __init__(
        self,
        project_root=None,
        host="0.0.0.0",
        port=8787,
        codex_home=None,
        sessions_file=None,
        limit=5,
        interval=5.0,
        log_callback=None,
    ):
        self.project_root = Path(project_root) if project_root else default_project_root()
        self.host = host
        self.port = int(port)
        self.codex_home = codex_home
        self.sessions_file = Path(sessions_file) if sessions_file else self.project_root / "sessions.json"
        self.limit = int(limit)
        self.interval = float(interval)
        self.log_callback = log_callback
        self._stop_event = threading.Event()
        self._exporter_thread = None
        self._bridge_thread = None
        self._server = None
        self._lock = threading.Lock()
        self._error = ""
        self._export_cycle = 0

    @property
    def status_url(self):
        return f"http://127.0.0.1:{self.port}/status"

    @property
    def lan_urls(self):
        return [f"http://{address}:{self.port}/status" for address in local_ipv4_addresses()]

    @property
    def error(self):
        with self._lock:
            return self._error

    def is_running(self):
        return bool(
            self._exporter_thread
            and self._exporter_thread.is_alive()
            and self._bridge_thread
            and self._bridge_thread.is_alive()
        )

    def start(self):
        if self.is_running():
            self._log("Codex Status 服务已经在运行。")
            return
        self._stop_event.clear()
        with self._lock:
            self._error = ""
        self.sessions_file.parent.mkdir(parents=True, exist_ok=True)

        self._server = StatusServer(
            host=self.host,
            port=self.port,
            sessions_file=str(self.sessions_file),
        )
        self.port = self._server.port
        self._exporter_thread = threading.Thread(target=self._run_exporter, name="codex-status-exporter", daemon=True)
        self._bridge_thread = threading.Thread(target=self._run_bridge, name="codex-status-bridge", daemon=True)
        self._exporter_thread.start()
        self._bridge_thread.start()
        self._log(f"服务已启动：{self.status_url}")
        for url in self.lan_urls:
            self._log(f"局域网地址：{url}")

    def stop(self, timeout=3.0):
        self._stop_event.set()
        server = self._server
        if server is not None:
            try:
                server.shutdown()
            except Exception as exc:
                self._set_error(f"停止 bridge 失败：{exc}")
            try:
                server.server_close()
            except Exception as exc:
                self._set_error(f"关闭 bridge socket 失败：{exc}")
        for thread in (self._exporter_thread, self._bridge_thread):
            if thread and thread.is_alive():
                thread.join(timeout=timeout)
        self._server = None
        self._exporter_thread = None
        self._bridge_thread = None
        self._log("服务已停止。")

    def _run_exporter(self):
        while not self._stop_event.is_set():
            try:
                payload = export_sessions(
                    codex_home=self.codex_home,
                    output_file=self.sessions_file,
                    limit=self.limit,
                )
                self._export_cycle += 1
                if self._export_cycle % 60 == 0:
                    self._log(f"已刷新 sessions：{len(payload['sessions'])} 个会话")
            except Exception as exc:
                self._set_error(f"刷新 sessions 失败：{exc}")
            self._stop_event.wait(self.interval)

    def _run_bridge(self):
        try:
            self._server.serve_forever(poll_interval=0.2)
        except Exception as exc:
            if not self._stop_event.is_set():
                self._set_error(f"bridge 服务异常：{exc}")

    def _set_error(self, message):
        with self._lock:
            self._error = message
        self._log(message)

    def _log(self, message):
        if self.log_callback:
            self.log_callback(message)


def default_project_root():
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parents[1]


def local_ipv4_addresses():
    addresses = []
    try:
        host_name = socket.gethostname()
        for info in socket.getaddrinfo(host_name, None, socket.AF_INET):
            address = info[4][0]
            if address != "127.0.0.1" and not address.startswith("169.254.") and address not in addresses:
                addresses.append(address)
    except OSError:
        return []
    return addresses
