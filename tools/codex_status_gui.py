import sys
from datetime import datetime

from PySide6.QtCore import QObject, QTimer, Qt, Signal
from PySide6.QtWidgets import (
    QApplication,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QPushButton,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

from tools.codex_status_service import CodexStatusService


class LogEmitter(QObject):
    message = Signal(str)


class CodexStatusWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Codex Status 服务")
        self.resize(560, 360)
        self.log_emitter = LogEmitter()
        self.log_emitter.message.connect(self.append_log)
        self.service = CodexStatusService(log_callback=self.log_emitter.message.emit)
        self.is_starting_or_stopping = False

        root = QWidget()
        layout = QVBoxLayout(root)

        title = QLabel("Codex Status 电脑端服务")
        title.setStyleSheet("font-size: 20px; font-weight: 600;")
        layout.addWidget(title)

        status_row = QHBoxLayout()
        status_row.addWidget(QLabel("状态："))
        self.status_label = QLabel("未启动")
        self.status_label.setStyleSheet("font-weight: 600; color: #8a6d3b;")
        status_row.addWidget(self.status_label, stretch=1)
        self.toggle_button = QPushButton("启动服务")
        self.toggle_button.clicked.connect(self.toggle_service)
        status_row.addWidget(self.toggle_button)
        layout.addLayout(status_row)

        self.url_label = QLabel(f"本机地址：{self.service.status_url}")
        self.url_label.setTextInteractionFlags(Qt.TextSelectableByMouse)
        layout.addWidget(self.url_label)

        self.lan_label = QLabel("局域网地址：启动后显示")
        self.lan_label.setWordWrap(True)
        self.lan_label.setTextInteractionFlags(Qt.TextSelectableByMouse)
        layout.addWidget(self.lan_label)

        self.log_view = QTextEdit()
        self.log_view.setReadOnly(True)
        layout.addWidget(self.log_view, stretch=1)

        self.setCentralWidget(root)

        self.status_timer = QTimer(self)
        self.status_timer.timeout.connect(self.refresh_status)
        self.status_timer.start(500)

    def toggle_service(self):
        if self.is_starting_or_stopping:
            return
        if self.service.is_running():
            self.stop_service()
        else:
            self.start_service()

    def start_service(self):
        self.is_starting_or_stopping = True
        self.set_status("启动中", "#31708f")
        self.toggle_button.setEnabled(False)
        try:
            self.service.start()
        except Exception as exc:
            self.append_log(f"启动失败：{exc}")
            self.set_status("错误", "#a94442")
        finally:
            self.is_starting_or_stopping = False
            self.toggle_button.setEnabled(True)
            self.refresh_status()

    def stop_service(self):
        self.is_starting_or_stopping = True
        self.set_status("停止中", "#31708f")
        self.toggle_button.setEnabled(False)
        try:
            self.service.stop()
        finally:
            self.is_starting_or_stopping = False
            self.toggle_button.setEnabled(True)
            self.refresh_status()

    def refresh_status(self):
        if self.is_starting_or_stopping:
            return
        if self.service.error:
            self.set_status("错误", "#a94442")
        elif self.service.is_running():
            self.set_status("运行中", "#3c763d")
        else:
            self.set_status("未启动", "#8a6d3b")
        self.toggle_button.setText("停止服务" if self.service.is_running() else "启动服务")
        self.url_label.setText(f"本机地址：{self.service.status_url}")
        lan_urls = self.service.lan_urls
        self.lan_label.setText("局域网地址：" + ("  ".join(lan_urls) if lan_urls else "未检测到"))

    def set_status(self, text, color):
        self.status_label.setText(text)
        self.status_label.setStyleSheet(f"font-weight: 600; color: {color};")

    def append_log(self, message):
        timestamp = datetime.now().strftime("%H:%M:%S")
        self.log_view.append(f"[{timestamp}] {message}")

    def closeEvent(self, event):
        if self.service.is_running():
            self.append_log("窗口关闭，正在停止服务...")
            self.service.stop()
        event.accept()


def main():
    app = QApplication(sys.argv)
    window = CodexStatusWindow()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
