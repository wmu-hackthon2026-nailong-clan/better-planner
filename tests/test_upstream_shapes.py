"""Fixtures matching the exact upstream JSON shapes, no real login.

Covers the three properties the sync pipeline must never regress on:
field mapping, dedupe (identity + deadline change), and completion status.
"""
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from app.connectors import Syncer
from app.core import Store, normalize

HW = [{"course": "生物化学", "title": "第三章课后练习", "status": "待提交",
       "deadline": "2026-11-02 23:59", "url": "https://mooc1.chaoxing.com/x"}]
EXAM = [{"name": "生物化学", "start": "2026-11-10 09:00", "end": "2026-11-10 11:00",
         "durationMin": "120", "status": "未开始", "answer": "", "score": "", "mode": "在线"}]
WEEK = [{"date": "2026-10-30", "start": "08:00", "end": "09:40", "name": "线性代数",
         "location": "茶山校区 7AJ302", "teacher": "范赛荣"}]
ZHS = {"events": [{"external_id": "zhs-1", "kind": "homework", "title": "章节作业",
                   "course": "医学心理学", "deadline": "2026-11-04 20:00"}], "errors": []}


class UpstreamShapeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.store = Store(Path(self.temp.name) / "test.sqlite3")

    def tearDown(self):
        self.temp.cleanup()

    def test_hw_fields_map_to_deadline(self):
        row = normalize(HW[0], "chaoxing", "homework")
        self.assertEqual(row["kind"], "homework")
        self.assertEqual(row["title"], "第三章课后练习")
        self.assertEqual(row["course"], "生物化学")
        self.assertEqual(row["end"], "2026-11-02T23:59:00+08:00")
        self.assertEqual(row["url"], "https://mooc1.chaoxing.com/x")
        self.assertFalse(row["platform_done"])

    def test_hw_submitted_status_marks_completed(self):
        for mark in ("已提交", "待批阅", "已批阅", "已完成"):
            row = normalize(dict(HW[0], status=mark), "chaoxing", "homework")
            self.assertTrue(row["platform_done"], mark)

    def test_exam_fields_map_to_schedule(self):
        row = normalize(EXAM[0], "chaoxing", "exam")
        self.assertEqual(row["kind"], "exam")
        self.assertEqual(row["start"], "2026-11-10T09:00:00+08:00")
        self.assertEqual(row["end"], "2026-11-10T11:00:00+08:00")
        self.assertFalse(row["platform_done"])

    def test_exam_answer_marks_completed(self):
        row = dict(EXAM[0], status=EXAM[0].get("answer") or EXAM[0].get("status") or "")
        row = dict(row, answer="已提交")
        row = dict(row, status=row.get("answer") or row.get("status") or "")
        self.assertTrue(normalize(row, "chaoxing", "exam")["platform_done"])

    def test_hw_deadline_change_updates_same_record(self):
        self.store.merge([normalize(HW[0], "chaoxing", "homework")])
        self.store.merge([normalize(dict(HW[0], deadline="2026-11-05 23:59"), "chaoxing", "homework")])
        events = self.store.state()["events"]
        self.assertEqual(len(events), 1)
        self.assertIn("11-05", events[0]["end"])

    def test_hw_url_not_https_is_dropped(self):
        row = normalize(dict(HW[0], url="javascript:alert(1)"), "chaoxing", "homework")
        self.assertEqual(row["url"], "")


class SyncerFixtureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.store = Store(Path(self.temp.name) / "test.sqlite3")

    def tearDown(self):
        self.temp.cleanup()

    def test_exact_upstream_payloads_flow_through(self):
        payloads = [WEEK, HW, [dict(e, answer="已提交") for e in EXAM], ZHS]
        with patch("app.connectors.config", return_value={}), \
             patch("app.connectors.run_json", side_effect=payloads) as runner:
            sync = Syncer(self.store)
            sync.lock.acquire()
            sync._run()
        events = self.store.state()["events"]
        self.assertEqual(len(events), 4)
        by_kind = {e["kind"]: e for e in events}
        self.assertIn("class", by_kind)
        self.assertIn("homework", by_kind)
        self.assertIn("exam", by_kind)
        self.assertEqual(by_kind["class"]["teacher"], "范赛荣")
        self.assertTrue(by_kind["exam"]["completed"])
        self.assertEqual(runner.call_args_list[1].args[0][-2:], ["--all-courses", "--json"])


if __name__ == "__main__":
    unittest.main()
