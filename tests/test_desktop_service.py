import json
import sqlite3
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import patch
from urllib.request import Request, urlopen
from urllib.error import HTTPError

from app.core import Store, normalize
from app.connectors import Syncer
from app.desktop_service import desktop_events, make_server


class DesktopTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.server = make_server(0, Path(self.temp.name) / "calendar.sqlite3")
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.server.auth.close()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.temp.cleanup()

    def state(self):
        with urlopen(self.url + "/api/state") as response:
            return json.load(response)

    def post(self, path, body, token=None, extra=None):
        headers = {"Content-Type": "application/json", "X-Planner-Token": token or self.state()["token"]}
        headers.update(extra or {})
        return urlopen(Request(self.url + path, data=json.dumps(body).encode(), headers=headers))

    def test_connection_page_and_scripts_are_served(self):
        for path, content in [("/", "学习通"), ("/connections.js", "X-Planner-Token"), ("/connections.css", ".platforms")]:
            with urlopen(self.url + path) as response:
                self.assertIn(content, response.read().decode())
                self.assertIn("frame-ancestors 'none'", response.headers["Content-Security-Policy"])

    def test_host_origin_and_token_are_checked(self):
        with self.assertRaises(HTTPError) as failure:
            urlopen(Request(self.url + "/api/state", headers={"Host": "attacker.invalid"}))
        self.assertEqual(failure.exception.code, 403)
        for token, headers in [("wrong", {}), (None, {"Origin": "https://attacker.invalid"})]:
            with self.assertRaises(HTTPError) as failure:
                self.post("/api/sync", {"provider": "chaoxing"}, token, headers)
            self.assertEqual(failure.exception.code, 403)

    def test_login_routes_to_provider_without_echoing_credentials(self):
        payload = dict(provider="chaoxing", phone="test-user", password="test-secret", remember=False)
        with patch("app.desktop_service.dependencies", return_value={"missing": [], "ready": True}), patch.object(self.server.auth, "start", return_value=True) as start:
            with self.post("/api/auth/login", payload) as response:
                body = response.read().decode()
                self.assertNotIn("test-secret", body)
                self.assertEqual(response.status, 202)
            self.assertEqual(start.call_args.args[0], "chaoxing")
            self.assertEqual(start.call_args.args[1]["password"], "test-secret")
        self.assertNotIn("test-secret", json.dumps(self.state()))

    def test_sync_routes_to_selected_platform_and_rejects_login_in_progress(self):
        with patch("app.desktop_service.dependencies", return_value={"missing": [], "ready": True}), patch.object(self.server.syncer, "start", return_value=True) as start:
            with self.post("/api/sync", {"provider": "zhihuishu"}) as response:
                self.assertEqual(response.status, 202)
            start.assert_called_once_with("zhihuishu")
            self.server.auth.jobs["zhihuishu"] = {"status": "qr", "message": "waiting"}
            with self.assertRaises(HTTPError) as failure:
                self.post("/api/sync", {"provider": "zhihuishu"})
            self.assertEqual(failure.exception.code, 400)

    def test_qr_is_present_in_browser_state_but_not_desktop_snapshot(self):
        self.server.auth.jobs["zhihuishu"] = {"status": "qr", "message": "scan", "image": "cXJjb2Rl"}
        self.assertEqual(self.state()["auth"]["zhihuishu"]["image"], "cXJjb2Rl")
        events, _ = desktop_events(self.server.store.state()["events"])
        self.assertNotIn("cXJjb2Rl", json.dumps(events))

    def test_timestamp_conversion_deduplication_and_unknown_deadline(self):
        rows = [normalize(dict(id="class-1", name="数学", date="2026-11-02", start="08:00", end="09:30"), "chaoxing", "class"),
                normalize(dict(id="task-1", title="作业", start="2026-10-01 10:00", end="2026-11-02 20:00"), "zhihuishu"),
                normalize(dict(id="unknown", title="时间未知"), "zhihuishu")]
        self.server.store.merge(rows)
        self.server.store.merge(rows)
        events, undated = desktop_events(self.server.store.state()["events"])
        self.assertEqual(len(events), 2)
        self.assertEqual(undated, 1)
        course = next(e for e in events if e["kind"] == "schedule")
        deadline = next(e for e in events if e["kind"] == "deadline")
        self.assertEqual(course["start"], 1793577600)
        self.assertEqual(course["end"] - course["start"], 5400)
        self.assertEqual(deadline["start"], deadline["end"])
        rows[1]["end"] = "2026-11-03T20:00:00+08:00"
        self.server.store.merge([rows[1]])
        updated, _ = desktop_events(self.server.store.state()["events"])
        task = next(e for e in updated if e["kind"] == "deadline")
        self.assertEqual(task["id"], deadline["id"])
        self.assertEqual(task["end"] - deadline["end"], 86400)

    def test_selected_platform_sync_does_not_query_other_platform(self):
        sync = Syncer(self.server.store)
        with patch("app.connectors.config", return_value={"xxt_command": ["python", "xxt.py"]}), patch("app.connectors.run_json", return_value=[]) as run:
            sync.lock.acquire()
            sync._run("chaoxing")
            self.assertEqual(run.call_count, 3)
            self.assertFalse(any("zhs_readonly" in str(c) for c in run.call_args_list))
        with patch("app.connectors.config", return_value={}), patch("app.connectors.run_json", return_value={"events": [], "errors": []}) as run:
            sync.lock.acquire()
            sync._run("zhihuishu")
            run.assert_called_once()
            self.assertIn("zhs_readonly.py", str(run.call_args))

    def test_failure_keeps_saved_data_and_surfaces_error(self):
        self.server.store.merge([normalize(dict(id="old", title="旧作业", end="2026-11-02 20:00"), "chaoxing")])
        sync = Syncer(self.server.store)
        with patch("app.connectors.config", return_value={}), patch("app.connectors.run_json", side_effect=RuntimeError("尚未登录")):
            sync.lock.acquire()
            sync._run("chaoxing")
        state = self.server.store.state()
        self.assertEqual(len(state["events"]), 1)
        self.assertTrue(all(row["error"] == "尚未登录" for row in state["sync"]))

    def test_report_counts_new_changed_unchanged_and_deduplicated_rows(self):
        row = normalize(dict(id="one", title="数学作业", course="数学", end="2026-11-02 20:00"), "chaoxing")
        first = self.server.store.merge([row, row])
        self.assertEqual((first["received"], first["added"], len(first["items"])), (2, 1, 1))
        self.server.store.complete(row["id"], True)
        second = self.server.store.merge([row])
        self.assertEqual(second["unchanged"], 1)
        changed = dict(row, end="2026-11-03T20:00:00+08:00")
        third = self.server.store.merge([changed])
        self.assertEqual(third["updated"], 1)
        self.assertTrue(third["items"][0]["calendar"])
        self.assertTrue(self.server.store.state()["events"][0]["completed"])

    def test_partial_report_persists_and_failure_does_not_reuse_success_counts(self):
        sync = Syncer(self.server.store)
        payload = {"events": [dict(id="a", title="无日期作业", course="英语")], "errors": ["另一课程查询失败"]}
        with patch("app.connectors.config", return_value={}), patch("app.connectors.run_json", return_value=payload):
            sync.lock.acquire()
            sync._run("zhihuishu")
        row = self.state()["sync"][0]
        self.assertEqual(row["report"]["status"], "partial")
        self.assertEqual(row["report"]["added"], 1)
        self.assertFalse(row["report"]["items"][0]["calendar"])
        persisted = Store(self.server.store.path).state()["sync"][0]
        self.assertEqual(persisted["report"], row["report"])
        with patch("app.connectors.config", return_value={}), patch("app.connectors.run_json", side_effect=RuntimeError("登录过期")):
            sync.lock.acquire()
            sync._run("zhihuishu")
        row = self.state()["sync"][0]
        self.assertIsNone(row["report"])
        self.assertEqual(row["error"], "登录过期")
        self.assertEqual(len(self.state()["events"]), 1)

    def test_empty_success_is_an_explicit_zero_report(self):
        sync = Syncer(self.server.store)
        with patch("app.connectors.config", return_value={}), patch("app.connectors.run_json", return_value=[]):
            sync.lock.acquire()
            sync._run("chaoxing")
        for row in self.state()["sync"]:
            self.assertEqual(row["report"]["status"], "success")
            self.assertEqual(row["report"]["items"], [])
            self.assertEqual(row["report"]["added"], 0)

    def test_existing_database_sync_history_migrates_without_losing_records(self):
        path = Path(self.temp.name) / "old.sqlite3"
        with sqlite3.connect(path) as db:
            db.execute("CREATE TABLE sync_state(stream TEXT PRIMARY KEY, success_at TEXT, attempted_at TEXT, error TEXT)")
            db.execute("INSERT INTO sync_state VALUES('学习通课表','2026-11-01','2026-11-01',NULL)")
        db.close()
        store = Store(path)
        self.assertEqual(store.state()["sync"][0]["success_at"], "2026-11-01")
        self.assertIsNone(store.state()["sync"][0]["report"])
        store.record_sync("学习通课表", report={"items": [], "added": 0})
        self.assertEqual(store.state()["sync"][0]["report"]["added"], 0)

    def test_known_legacy_ai_identity_is_adopted_without_duplicate_or_lost_completion(self):
        old = normalize(dict(id="ai:course:task:exam", kind="exam", title="自测", course="生理学"), "zhihuishu")
        new = normalize(dict(id="ai:course:class:task:task", kind="homework", title="自测", course="生理学"), "zhihuishu")
        self.server.store.merge([old])
        self.server.store.complete(old["id"], True)
        report = self.server.store.merge([new], aliases={new["id"]: [old["id"]]})
        saved = self.server.store.state()["events"]
        self.assertEqual(len(saved), 1)
        self.assertEqual(saved[0]["kind"], "homework")
        self.assertTrue(saved[0]["completed"])
        self.assertEqual(report["updated"], 1)
        self.assertEqual(self.server.store.merge([new])["unchanged"], 1)


if __name__ == "__main__":
    unittest.main()
