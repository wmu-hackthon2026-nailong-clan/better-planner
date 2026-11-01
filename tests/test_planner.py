import json
import tempfile
import threading
import unittest
from datetime import datetime
from pathlib import Path
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from app.core import Store, calendar, date_value, normalize
from app.connectors import Syncer, run_json
from app.server import make_server
from scripts.zhs_readonly import homework_pages


class PaginationTests(unittest.TestCase):
    def test_empty_page_stops_even_with_truthy_next(self):
        session = type('Session', (), {'urls': type('URLs', (), {'homework': 'https://example.test'})(),
                                     'homework_query': lambda *args: {'rt': {'next': 'true', 'studentHomeworkList': []}}})()
        self.assertEqual(list(homework_pages(session, 'c', 'r', 1)), [])

    def test_next_only_false_is_empty_list(self):
        session = type('Session', (), {'urls': type('URLs', (), {'homework': 'https://example.test'})(),
                                     'homework_query': lambda *args: {'rt': {'next': 'false'}}})()
        self.assertEqual(list(homework_pages(session, 'c', 'r', 2)), [])


class PlannerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.store = Store(Path(self.temp.name) / "test.sqlite3")

    def tearDown(self):
        self.temp.cleanup()

    def test_deadline_change_updates_same_task_and_retains_local_completion(self):
        a = normalize(dict(title="第三章作业", course="高等数学", deadline="2026-11-01 23:59"), "chaoxing")
        self.store.merge([a])
        self.store.complete(a["id"], True)
        b = normalize(dict(title="第三章作业", course="高等数学", deadline="2026-11-03 23:59"), "chaoxing")
        self.store.merge([b, b])
        events = self.store.state()["events"]
        self.assertEqual(len(events), 1)
        self.assertTrue(events[0]["completed"])
        self.assertIn("11-03", events[0]["end"])

    def test_platform_and_course_identity_do_not_collide(self):
        rows = [normalize(dict(title="测验", course=c), source)
                for c in ("数学", "英语") for source in ("chaoxing", "zhihuishu")]
        self.store.merge(rows)
        self.assertEqual(len(self.store.state()["events"]), 4)

    def test_unknown_deadline_and_timezone(self):
        self.assertIsNone(date_value("截止未知"))
        self.assertEqual(date_value("2026-10-30T12:00:00Z"), "2026-10-30T20:00:00+08:00")
        self.assertEqual(date_value("2026-10-30 12:00"), "2026-10-30T12:00:00+08:00")
        with self.assertRaises(ValueError):
            date_value("10-30 12:00")

    def test_xxt_week_shape(self):
        row = normalize(dict(name="线性代数", date="2026-10-30", start="08:00", end="09:40", location="A301"), "chaoxing", "class")
        self.assertEqual(row["start"], "2026-10-30T08:00:00+08:00")
        self.assertEqual(row["end"], "2026-10-30T09:40:00+08:00")
        self.assertEqual(row["course"], "线性代数")

    def test_invalid_import_is_not_partially_committed(self):
        with self.assertRaises(ValueError):
            self.store.merge([normalize(x, "zhihuishu") for x in [dict(title="有效"), dict(title="")]])
        self.assertEqual(self.store.state()["events"], [])

    def test_untrusted_link_is_removed(self):
        row = normalize(dict(title="作业", url="javascript:alert(1)"), "manual")
        self.assertEqual(row["url"], "")

    def test_ics_utf8_folding_escaping_and_timezone(self):
        self.store.merge([normalize(dict(title="中文标题"*30+",;\n说明", start="2026-10-30 08:00", end="2026-10-30 09:40", kind="class"), "manual")])
        text = calendar(self.store.state()["events"])
        self.assertIn("DTSTART:20261030T000000Z", text)
        self.assertIn("\\,\\;\\n", text.replace("\r\n ", ""))
        self.assertTrue(all(len(s.encode()) <= 75 for s in text.split("\r\n")))

    def test_ics_tasks_appear_only_on_day_before_deadline(self):
        self.store.merge([
            normalize(dict(title='长时间开放的考试', kind='exam', start='2026-12-01 08:00', end='2027-01-01 23:59'), 'zhihuishu'),
            normalize(dict(title='跨月作业', end='2026-11-01 00:01'), 'chaoxing'),
        ])
        text = calendar(self.store.state()['events']).replace('\r\n ', '')
        self.assertIn('DTSTART;VALUE=DATE:20261231', text)
        self.assertIn('DTEND;VALUE=DATE:20270101', text)
        self.assertIn('DTSTART;VALUE=DATE:20261031', text)
        self.assertIn('明日截止 · 长时间开放的考试', text)
        self.assertIn('实际截止：2027-01-01 23:59', text)
        self.assertNotIn('20261201T', text)

    def test_failed_sync_keeps_events_and_success_timestamp(self):
        self.store.merge([normalize(dict(title="上次作业"), "chaoxing")])
        self.store.record_sync("学习通作业")
        stamp = self.store.state()["sync"][0]["success_at"]
        with patch("app.connectors.config", return_value={}), patch("app.connectors.run_json", side_effect=RuntimeError("模拟登录失效")):
            sync = Syncer(self.store)
            sync.lock.acquire()
            sync._run()
        state = self.store.state()
        self.assertEqual(len(state["events"]), 1)
        hw = next(x for x in state["sync"] if x["stream"] == "学习通作业")
        self.assertEqual(hw["success_at"], stamp)
        self.assertIn("登录失效", hw["error"])
        self.assertFalse(sync.running)

    def test_platform_submitted_cannot_be_undone_by_local_toggle(self):
        row = normalize(dict(title="已交作业", status="待批阅"), "chaoxing")
        self.store.merge([row])
        self.store.complete(row["id"], False)
        self.assertTrue(self.store.state()["events"][0]["completed"])

    def test_adapter_happy_path_merges_both_platforms(self):
        payloads = [
            [dict(name="高等数学", date="2026-10-30", start="08:00", end="09:40")],
            [dict(title="习题", course="高等数学", deadline="2026-11-01 20:00")],
            [dict(name="测验", start="2026-11-02 10:00", end="2026-11-02 11:00", answer="已提交")],
            dict(events=[dict(external_id="zhs-1", title="章节作业", kind="homework", deadline="2026-11-04 20:00")], errors=[]),
        ]
        with patch("app.connectors.config", return_value={}), patch("app.connectors.run_json", side_effect=payloads) as runner:
            sync = Syncer(self.store)
            sync.lock.acquire()
            sync._run()
        state = self.store.state()
        self.assertEqual(len(state["events"]), 4)
        self.assertEqual(len(state["sync"]), 4)
        self.assertTrue(all(not s["error"] for s in state["sync"]))
        self.assertTrue(next(e for e in state["events"] if e["kind"] == "exam")["completed"])
        self.assertIn("--all-courses", runner.call_args_list[1].args[0])

    def test_partial_xxt_fetch_is_never_reported_as_success(self):
        fake = type("Result", (), dict(returncode=0, stdout='[{"title":"有效作业"}]', stderr="这些课的作业没拉到"))()
        with patch("app.connectors.subprocess.run", return_value=fake):
            value = run_json(["xxt", "hw", "--json"])
        self.assertEqual(value["events"][0]["title"], "有效作业")
        self.assertTrue(value["errors"])

    def test_partial_sync_saves_valid_tasks_with_warning(self):
        payloads = [[], {'events': [{'title': '有效作业'}], 'errors': ['部分课程失败']}, [], {'events': [], 'errors': []}]
        with patch('app.connectors.config', return_value={}), patch('app.connectors.run_json', side_effect=payloads):
            sync = Syncer(self.store)
            sync.lock.acquire()
            sync._run()
        state = self.store.state()
        self.assertEqual(state['events'][0]['title'], '有效作业')
        self.assertTrue(next(s for s in state['sync'] if s['stream'] == '学习通作业')['error'])


class HTTPTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.server = make_server(0, Path(cls.temp.name) / "http.sqlite3")
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.base = f"http://127.0.0.1:{cls.server.server_port}"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()
        cls.temp.cleanup()

    def request(self, path, data=None, token=None, extra=None):
        headers = {"Content-Type": "application/json"}
        if token:
            headers["X-Planner-Token"] = token
        headers.update(extra or {})
        req = Request(self.base+path, data=json.dumps(data).encode() if data is not None else None, headers=headers)
        return urlopen(req, timeout=5)

    def test_html_assets_and_missing_route(self):
        for route in ("/", "/app.js", "/style.css"):
            with self.request(route) as response:
                self.assertEqual(response.status, 200)
                self.assertIn("frame-ancestors 'none'", response.headers["Content-Security-Policy"])
        with self.assertRaises(HTTPError) as error:
            self.request("/../config.local.json")
        self.assertEqual(error.exception.code, 404)
        error.exception.close()

    def test_mutations_require_session_token_and_local_host(self):
        with self.assertRaises(HTTPError) as error:
            self.request("/api/events", dict(title="非法请求"))
        self.assertEqual(error.exception.code, 403)
        error.exception.close()
        with self.assertRaises(HTTPError) as error:
            self.request("/api/state", extra={"Host": "untrusted.example"})
        self.assertEqual(error.exception.code, 403)
        error.exception.close()

    def test_create_complete_export_delete_roundtrip(self):
        with self.request("/api/state") as r:
            token = json.load(r)["token"]
        self.request("/api/events", dict(title="HTTP 集成测试", course="测试课程", end="2026-11-05 20:00"), token).close()
        with self.request("/api/state") as r:
            event = next(e for e in json.load(r)["events"] if e["title"] == "HTTP 集成测试")
        with self.request("/api/calendar.ics") as r:
            self.assertIn("HTTP 集成测试", r.read().decode())
        self.request("/api/complete", dict(id=event["id"], done=True), token).close()
        with self.request("/api/state") as r:
            self.assertTrue(next(e for e in json.load(r)["events"] if e["id"] == event["id"])["completed"])
        self.request("/api/delete", dict(id=event["id"]), token).close()


if __name__ == "__main__":
    unittest.main()
