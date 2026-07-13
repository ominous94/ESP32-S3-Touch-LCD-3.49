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

    def test_rate_limit_notification_merges_sparse_window(self):
        client = CodexAppServerStatus(command=["unused"])
        client._rate_limits = {
            "rateLimits": {
                "limitId": "codex",
                "primary": {
                    "usedPercent": 25,
                    "windowDurationMins": 300,
                    "resetsAt": 1784490776,
                },
            }
        }

        client.handle_notification(
            "account/rateLimits/updated",
            {
                "rateLimits": {
                    "limitId": "codex",
                    "primary": {"usedPercent": 31, "resetsAt": None},
                }
            },
        )

        window = client.rate_limits_snapshot()["rateLimits"]["primary"]
        self.assertEqual(window["usedPercent"], 31)
        self.assertEqual(window["windowDurationMins"], 300)
        self.assertEqual(window["resetsAt"], 1784490776)

    def test_model_bucket_notification_does_not_replace_main_codex_limit(self):
        client = CodexAppServerStatus(command=["unused"])
        client._rate_limits = {
            "rateLimits": {
                "limitId": "codex",
                "primary": {"usedPercent": 25, "windowDurationMins": 300},
            },
            "rateLimitsByLimitId": {
                "codex": {
                    "limitId": "codex",
                    "primary": {"usedPercent": 25, "windowDurationMins": 300},
                },
                "codex_other": {
                    "limitId": "codex_other",
                    "primary": {"usedPercent": 10, "windowDurationMins": 60},
                },
            },
        }

        client.handle_notification(
            "account/rateLimits/updated",
            {
                "rateLimits": {
                    "limitId": "codex_other",
                    "primary": {"usedPercent": 42},
                }
            },
        )

        snapshot = client.rate_limits_snapshot()
        self.assertEqual(snapshot["rateLimits"]["limitId"], "codex")
        self.assertEqual(snapshot["rateLimits"]["primary"]["usedPercent"], 25)
        self.assertEqual(
            snapshot["rateLimitsByLimitId"]["codex_other"]["primary"]["usedPercent"],
            42,
        )


if __name__ == "__main__":
    unittest.main()
