import json
import os
import time
import unittest
from datetime import datetime, timezone
from pathlib import Path
from tempfile import TemporaryDirectory

from tools.export_codex_sessions import build_sessions_payload, export_sessions


STATUS_TITLE = "\u72b6\u6001\u5c4f\u5f00\u53d1"
OTHER_TITLE = "\u771f\u673a\u6d4b\u8bd5"
WORKING_ZH = "\u5de5\u4f5c\u4e2d"
IDLE_ZH = "\u672a\u8fd0\u884c"
DONE_ZH = "\u5df2\u5b8c\u6210"
USER_DETAIL = "\u7528\u6237\uff1a\u6253\u5f00\u8be6\u60c5\u9875"
ASSISTANT_DETAIL = "\u52a9\u624b\uff1a\u6b63\u5728\u5236\u5b9a\u5b9e\u73b0\u65b9\u6848"


class ExportCodexSessionsTests(unittest.TestCase):
    def test_build_sessions_payload_reads_recent_codex_session_index(self):
        with TemporaryDirectory() as temp_dir:
            codex_home = Path(temp_dir) / ".codex"
            codex_home.mkdir()
            (codex_home / "session_index.jsonl").write_text(
                "\n".join(
                    [
                        json.dumps(
                            {
                                "id": "thread-old",
                                "thread_name": OTHER_TITLE,
                                "updated_at": "2026-06-12T01:30:00.1111111Z",
                            },
                            ensure_ascii=False,
                        ),
                        json.dumps(
                            {
                                "id": "thread-new",
                                "thread_name": STATUS_TITLE,
                                "updated_at": "2026-06-12T03:30:00.2222222Z",
                            },
                            ensure_ascii=False,
                        ),
                    ]
                ),
                encoding="utf-8",
            )
            (codex_home / ".codex-global-state.json").write_text(
                json.dumps(
                    {
                        "active-workspace-roots": ["F:\\CodexProject\\ESP32-S3-Touch-LCD-3.49"],
                        "thread-workspace-root-hints": {
                            "thread-new": "F:\\CodexProject\\ESP32-S3-Touch-LCD-3.49",
                            "thread-old": "F:\\GamePlayerCom",
                        },
                    },
                    ensure_ascii=False,
                ),
                encoding="utf-8",
            )

            payload = build_sessions_payload(codex_home=codex_home, limit=2)

        self.assertEqual(len(payload["sessions"]), 2)
        self.assertEqual(payload["sessions"][0]["title"], STATUS_TITLE)
        self.assertEqual(payload["sessions"][0]["state"], "notLoaded")
        self.assertEqual(payload["sessions"][0]["status_zh"], IDLE_ZH)
        self.assertEqual(payload["sessions"][0]["cwd"], "ESP32-S3-Touch-LCD-3.49")
        self.assertEqual(payload["sessions"][1]["title"], OTHER_TITLE)
        self.assertEqual(payload["sessions"][1]["state"], "notLoaded")
        self.assertEqual(payload["sessions"][1]["status_zh"], IDLE_ZH)
        self.assertEqual(payload["sessions"][1]["cwd"], "GamePlayerCom")

    def test_export_sessions_writes_bridge_compatible_json(self):
        with TemporaryDirectory() as temp_dir:
            codex_home = Path(temp_dir) / ".codex"
            output_file = Path(temp_dir) / "sessions.json"
            codex_home.mkdir()
            (codex_home / "session_index.jsonl").write_text(
                json.dumps(
                    {
                        "id": "thread-one",
                        "thread_name": STATUS_TITLE,
                        "updated_at": "2026-06-12T03:30:00Z",
                    },
                    ensure_ascii=False,
                ),
                encoding="utf-8",
            )

            export_sessions(codex_home=codex_home, output_file=output_file, limit=5)
            payload = json.loads(output_file.read_text(encoding="utf-8"))

        self.assertEqual(payload["sessions"][0]["title"], STATUS_TITLE)
        self.assertEqual(payload["sessions"][0]["state"], "notLoaded")

    def test_build_sessions_payload_marks_task_complete_rollout_as_complete(self):
        with TemporaryDirectory() as temp_dir:
            codex_home = Path(temp_dir) / ".codex"
            rollout_dir = codex_home / "sessions" / "2026" / "06" / "14"
            rollout_dir.mkdir(parents=True)
            (rollout_dir / "rollout-2026-06-14T09-00-00-thread-done.jsonl").write_text(
                "\n".join(
                    [
                        json.dumps(
                            {
                                "timestamp": "2026-06-14T09:00:00.000Z",
                                "type": "session_meta",
                                "payload": {
                                    "id": "thread-done",
                                    "cwd": "F:\\CodexProject\\ESP32-S3-Touch-LCD-3.49",
                                },
                            },
                            ensure_ascii=False,
                        ),
                        json.dumps(
                            {
                                "timestamp": "2026-06-14T09:01:00.000Z",
                                "type": "response_item",
                                "payload": {
                                    "type": "message",
                                    "role": "user",
                                    "content": [{"type": "input_text", "text": STATUS_TITLE}],
                                },
                            },
                            ensure_ascii=False,
                        ),
                        json.dumps(
                            {
                                "timestamp": "2026-06-14T09:02:00.000Z",
                                "type": "event_msg",
                                "payload": {"type": "task_complete"},
                            },
                            ensure_ascii=False,
                        ),
                    ]
                ),
                encoding="utf-8",
            )

            payload = build_sessions_payload(
                codex_home=codex_home,
                now=datetime(2026, 6, 14, 9, 5, 0, tzinfo=timezone.utc),
                limit=1,
            )

        self.assertEqual(payload["sessions"][0]["title"], STATUS_TITLE)
        self.assertEqual(payload["sessions"][0]["state"], "complete")
        self.assertEqual(payload["sessions"][0]["status_zh"], DONE_ZH)

    def test_app_server_active_state_is_preferred(self):
        with TemporaryDirectory() as temp_dir:
            codex_home = Path(temp_dir) / ".codex"
            codex_home.mkdir()

            payload = build_sessions_payload(
                codex_home=codex_home,
                limit=1,
                app_server_threads=[
                    {
                        "id": "thread-app-server",
                        "name": STATUS_TITLE,
                        "cwd": str(codex_home.parent),
                        "updatedAt": 1781427600,
                        "status": {"type": "active", "activeFlags": []},
                    }
                ],
            )

        self.assertEqual(payload["source"], "appServer")
        self.assertEqual(payload["source_status"], "ok")
        self.assertEqual(payload["sessions"][0]["id"], "thread-app-server")
        self.assertEqual(payload["sessions"][0]["state"], "active")
        self.assertEqual(payload["sessions"][0]["status_source"], "appServer")

    def test_complete_session_older_than_ten_minutes_is_exported_as_not_loaded(self):
        with TemporaryDirectory() as temp_dir:
            codex_home = Path(temp_dir) / ".codex"
            rollout_dir = codex_home / "sessions" / "2026" / "06" / "14"
            rollout_dir.mkdir(parents=True)
            (rollout_dir / "rollout-2026-06-14T09-00-00-thread-stale-done.jsonl").write_text(
                "\n".join(
                    [
                        json.dumps(
                            {
                                "timestamp": "2026-06-14T09:00:00.000Z",
                                "type": "session_meta",
                                "payload": {"id": "thread-stale-done"},
                            },
                            ensure_ascii=False,
                        ),
                        json.dumps(
                            {
                                "timestamp": "2026-06-14T09:01:00.000Z",
                                "type": "response_item",
                                "payload": {
                                    "type": "message",
                                    "role": "user",
                                    "content": [{"type": "input_text", "text": STATUS_TITLE}],
                                },
                            },
                            ensure_ascii=False,
                        ),
                        json.dumps(
                            {
                                "timestamp": "2026-06-14T09:02:00.000Z",
                                "type": "event_msg",
                                "payload": {"type": "task_complete"},
                            },
                            ensure_ascii=False,
                        ),
                    ]
                ),
                encoding="utf-8",
            )

            payload = build_sessions_payload(
                codex_home=codex_home,
                now=datetime(2026, 6, 14, 9, 13, 0, tzinfo=timezone.utc),
                limit=1,
            )

        self.assertEqual(payload["sessions"][0]["state"], "notLoaded")
        self.assertEqual(payload["sessions"][0]["status_zh"], IDLE_ZH)

    def test_exported_titles_are_limited_for_two_line_display(self):
        long_title = "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        with TemporaryDirectory() as temp_dir:
            codex_home = Path(temp_dir) / ".codex"
            codex_home.mkdir()
            (codex_home / "session_index.jsonl").write_text(
                json.dumps(
                    {
                        "id": "thread-long",
                        "thread_name": long_title,
                        "updated_at": "2026-06-12T03:30:00Z",
                    }
                ),
                encoding="utf-8",
            )

            payload = build_sessions_payload(codex_home=codex_home, limit=1)

        title = payload["sessions"][0]["title"]
        self.assertLessEqual(len(title), 24)
        self.assertTrue(title.endswith("..."))

    def test_build_sessions_payload_includes_recent_rollout_not_yet_in_index(self):
        with TemporaryDirectory() as temp_dir:
            codex_home = Path(temp_dir) / ".codex"
            rollout_dir = codex_home / "sessions" / "2026" / "06" / "12"
            rollout_dir.mkdir(parents=True)
            (codex_home / "session_index.jsonl").write_text(
                json.dumps(
                    {
                        "id": "thread-indexed",
                        "thread_name": OTHER_TITLE,
                        "updated_at": "2026-06-12T01:30:00.1111111Z",
                    },
                    ensure_ascii=False,
                ),
                encoding="utf-8",
            )
            (codex_home / ".codex-global-state.json").write_text(
                json.dumps(
                    {"active-workspace-roots": ["F:\\CodexProject\\ESP32-S3-Touch-LCD-3.49"]},
                    ensure_ascii=False,
                ),
                encoding="utf-8",
            )
            (rollout_dir / "rollout-2026-06-12T03-23-42-thread-live.jsonl").write_text(
                "\n".join(
                    [
                        json.dumps(
                            {
                                "timestamp": "2026-06-12T03:23:42.542Z",
                                "type": "session_meta",
                                "payload": {
                                    "id": "thread-live",
                                    "cwd": "F:\\CodexProject\\ESP32-S3-Touch-LCD-3.49",
                                },
                            },
                            ensure_ascii=False,
                        ),
                        json.dumps(
                            {
                                "timestamp": "2026-06-12T03:24:00.000Z",
                                "type": "response_item",
                                "payload": {
                                    "type": "message",
                                    "role": "user",
                                    "content": [{"type": "input_text", "text": "<environment_context>ignored</environment_context>"}],
                                },
                            },
                            ensure_ascii=False,
                        ),
                        json.dumps(
                            {
                                "timestamp": "2026-06-12T03:25:00.000Z",
                                "type": "response_item",
                                "payload": {
                                    "type": "message",
                                    "role": "user",
                                    "content": [{"type": "input_text", "text": STATUS_TITLE}],
                                },
                            },
                            ensure_ascii=False,
                        ),
                        json.dumps(
                            {
                                "timestamp": "2026-06-12T03:26:00.000Z",
                                "type": "event_msg",
                                "payload": {"type": "turn_started"},
                            },
                            ensure_ascii=False,
                        ),
                    ]
                ),
                encoding="utf-8",
            )

            payload = build_sessions_payload(codex_home=codex_home, limit=2)

        self.assertEqual(payload["sessions"][0]["title"], STATUS_TITLE)
        self.assertEqual(payload["sessions"][0]["state"], "active")
        self.assertEqual(payload["sessions"][0]["cwd"], "ESP32-S3-Touch-LCD-3.49")
        self.assertEqual(payload["sessions"][1]["title"], OTHER_TITLE)

    def test_build_sessions_payload_prioritizes_active_sessions_before_limit(self):
        with TemporaryDirectory() as temp_dir:
            codex_home = Path(temp_dir) / ".codex"
            rollout_dir = codex_home / "sessions" / "2026" / "06" / "14"
            rollout_dir.mkdir(parents=True)

            completed_entries = []
            for index in range(5):
                completed_entries.append(
                    json.dumps(
                        {
                            "id": f"thread-done-{index}",
                            "thread_name": f"已完成会话 {index}",
                            "updated_at": f"2026-06-14T10:0{index}:00Z",
                        },
                        ensure_ascii=False,
                    )
                )
            (codex_home / "session_index.jsonl").write_text(
                "\n".join(completed_entries),
                encoding="utf-8",
            )

            for index in range(5):
                done_rollout = rollout_dir / f"rollout-2026-06-14T10-0{index}-00-thread-done-{index}.jsonl"
                done_rollout.write_text(
                    "\n".join(
                        [
                            json.dumps(
                                {
                                    "timestamp": f"2026-06-14T10:0{index}:00.000Z",
                                    "type": "session_meta",
                                    "payload": {"id": f"thread-done-{index}"},
                                },
                                ensure_ascii=False,
                            ),
                            json.dumps(
                                {
                                    "timestamp": f"2026-06-14T10:0{index}:10.000Z",
                                    "type": "event_msg",
                                    "payload": {"type": "task_complete"},
                                },
                                ensure_ascii=False,
                            ),
                        ]
                    ),
                    encoding="utf-8",
                )

            active_rollout = rollout_dir / "rollout-2026-06-14T09-00-00-thread-active.jsonl"
            active_rollout.write_text(
                "\n".join(
                    [
                        json.dumps(
                            {
                                "timestamp": "2026-06-14T09:00:00.000Z",
                                "type": "session_meta",
                                "payload": {"id": "thread-active"},
                            },
                            ensure_ascii=False,
                        ),
                        json.dumps(
                            {
                                "timestamp": "2026-06-14T09:00:10.000Z",
                                "type": "response_item",
                                "payload": {
                                    "type": "message",
                                    "role": "user",
                                    "content": [{"type": "input_text", "text": STATUS_TITLE}],
                                },
                            },
                            ensure_ascii=False,
                        ),
                        json.dumps(
                            {
                                "timestamp": "2026-06-14T09:00:20.000Z",
                                "type": "event_msg",
                                "payload": {"type": "turn_started"},
                            },
                            ensure_ascii=False,
                        ),
                    ]
                ),
                encoding="utf-8",
            )
            os.utime(active_rollout, (time.time(), time.time()))

            payload = build_sessions_payload(
                codex_home=codex_home,
                now=datetime(2026, 6, 14, 10, 5, 0, tzinfo=timezone.utc),
                limit=5,
            )

        self.assertEqual(len(payload["sessions"]), 5)
        self.assertEqual(payload["sessions"][0]["title"], STATUS_TITLE)
        self.assertEqual(payload["sessions"][0]["state"], "active")
        self.assertIn(
            "complete",
            {session["state"] for session in payload["sessions"][1:]},
        )

    def test_recent_rollout_is_explicitly_marked_as_fallback(self):
        with TemporaryDirectory() as temp_dir:
            codex_home = Path(temp_dir) / ".codex"
            rollout_dir = codex_home / "sessions" / "2026" / "06" / "14"
            rollout_dir.mkdir(parents=True)
            (rollout_dir / "rollout-2026-06-14T04-25-44-thread-detail.jsonl").write_text(
                "\n".join(
                    [
                        json.dumps(
                            {
                                "timestamp": "2026-06-14T04:25:44.000Z",
                                "type": "session_meta",
                                "payload": {
                                    "id": "thread-detail",
                                    "cwd": "F:\\CodexProject\\ESP32-S3-Touch-LCD-3.49",
                                },
                            },
                            ensure_ascii=False,
                        ),
                        json.dumps(
                            {
                                "timestamp": "2026-06-14T04:25:45.000Z",
                                "type": "response_item",
                                "payload": {
                                    "type": "message",
                                    "role": "user",
                                    "content": [{"type": "input_text", "text": "打开详情页"}],
                                },
                            },
                            ensure_ascii=False,
                        ),
                        json.dumps(
                            {
                                "timestamp": "2026-06-14T04:25:46.000Z",
                                "type": "response_item",
                                "payload": {
                                    "type": "message",
                                    "role": "assistant",
                                    "content": [{"type": "output_text", "text": "正在制定实现方案"}],
                                },
                            },
                            ensure_ascii=False,
                        ),
                    ]
                ),
                encoding="utf-8",
            )

            payload = build_sessions_payload(
                codex_home=codex_home,
                limit=1,
                app_server_threads=[],
            )

        self.assertEqual(payload["source_status"], "degraded")
        self.assertEqual(payload["sessions"][0]["status_source"], "rolloutFallback")


if __name__ == "__main__":
    unittest.main()
