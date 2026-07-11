import unittest

from tools.codex_app_server_status import (
    CodexAppServerStatus,
    app_server_thread_to_entry,
)


class CodexAppServerStatusTests(unittest.TestCase):
    def test_maps_runtime_and_approval_states(self):
        entry = app_server_thread_to_entry(
            {
                "id": "thread-active",
                "name": "状态适配器",
                "cwd": "F:\\CodexProject\\demo",
                "updatedAt": 1781427600,
                "status": {
                    "type": "active",
                    "activeFlags": ["waitingOnApproval"],
                },
            }
        )

        self.assertEqual(entry["app_server_state"], "active")
        self.assertEqual(entry["app_server_status_zh"], "等待处理")
        self.assertEqual(entry["status_source"], "appServer")

    def test_turn_failure_maps_to_blocked(self):
        entry = app_server_thread_to_entry(
            {
                "id": "thread-failed",
                "status": {"type": "idle"},
                "lastTurnStatus": "failed",
            }
        )

        self.assertEqual(entry["app_server_state"], "blocked")
        self.assertEqual(entry["app_server_status_zh"], "任务失败")

    def test_notifications_update_snapshot(self):
        client = CodexAppServerStatus(command=["unused"])
        client.handle_notification(
            "thread/status/changed",
            {
                "threadId": "thread-event",
                "status": {"type": "active", "activeFlags": []},
            },
        )
        client.handle_notification(
            "turn/completed",
            {
                "threadId": "thread-event",
                "turn": {"id": "turn-1", "status": "completed"},
            },
        )

        snapshot = client.snapshot()
        self.assertEqual(snapshot[0]["status"]["type"], "active")
        self.assertEqual(snapshot[0]["lastTurnStatus"], "completed")


if __name__ == "__main__":
    unittest.main()
