import importlib.util
import unittest
import json
from types import SimpleNamespace
from unittest.mock import Mock, patch

from scripts.zhs_ai_readonly import BASIC, TASKS, EXAMS, ai_query, collect_ai, task_event

HAS_ZHS = importlib.util.find_spec("zhs") is not None
ZH_SKIP = "需要先装连接器依赖（scripts/install-connectors.ps1），本机无 zhs 时跳过"


class AiReadonlyTests(unittest.TestCase):
    def setUp(self):
        self.course = dict(courseId="2094654518327537664", classId=260439, courseName="测试生理学")
        self.task = dict(id=827405, taskName="期中考试", taskType=2, status=1,
                         startTime="2026-11-03 09:47:19", endTime="2026-11-08 23:59:59",
                         examId=4680172, examTestId=4326448, examStatus=1)
        self.session = Mock()
        self.session.urls = SimpleNamespace(base="https://onlineservice-api.zhihuishu.com")
        self.session.zhidao_query.return_value = {"status": 200, "rt": [self.course]}

    def test_ai_exam_is_deduplicated_and_preserves_large_course_id_and_deadline(self):
        def query(_s, path, _params):
            return {BASIC: {"knowledgeStatistics": {"knowledgeCount": 134, "masteryPercentage": 5}},
                    TASKS: [self.task], EXAMS: [self.task]}[path]
        with patch("scripts.zhs_ai_readonly.ai_query", side_effect=query):
            result = collect_ai(self.session)
        self.assertEqual(len(result["events"]), 1)
        event = result["events"][0]
        self.assertEqual(event["kind"], "exam")
        self.assertEqual(event["deadline"], "2026-11-08 23:59:59")
        self.assertIn(self.course["courseId"], event["external_id"])
        self.assertFalse(event["completed"])
        self.assertEqual(result["courses"][0]["progress"]["masteryPercentage"], 5)
        self.assertEqual(result["errors"], [])

    def test_task_list_failure_still_reads_exam_list(self):
        def query(_s, path, _params):
            if path == TASKS:
                raise RuntimeError("failure")
            return [self.task] if path == EXAMS else {"knowledgeStatistics": {}}
        with patch("scripts.zhs_ai_readonly.ai_query", side_effect=query):
            result = collect_ai(self.session)
        self.assertEqual(len(result["events"]), 1)
        self.assertTrue(result["errors"])
        self.assertEqual(result["courses"][0]["tasks_status"], "failed")

    def test_failed_discovery_is_not_successful_empty_course_list(self):
        self.session.zhidao_query.return_value = {"code": 401, "rt": []}
        self.assertTrue(collect_ai(self.session)["errors"])

    def test_homework_unknown_status_is_not_assumed_completed(self):
        event = task_event(dict(self.task, taskType=1, status=3, endTime=None), self.course)
        self.assertEqual(event["kind"], "homework")
        self.assertIsNone(event["deadline"])
        self.assertFalse(event["completed"])

    def test_write_endpoints_are_rejected_before_any_network_call(self):
        with self.assertRaises(ValueError):
            ai_query(self.session, "stu/studyRecord/completed", {})
        self.session.api_query.assert_not_called()

    @unittest.skipUnless(HAS_ZHS, ZH_SKIP)
    def test_request_uses_capture_verified_date_and_encrypted_course_pair(self):
        from zhs.crypto import Cipher
        key, iv = b"0123456789abcdef", b"abcdef0123456789"
        self.session.crypto.key_bytes.side_effect = lambda name: key if name == "ai_key" else iv
        self.session.api_query.return_value = {"code": 200, "data": []}
        params = {"courseId": self.course["courseId"], "classId": "260439"}
        ai_query(self.session, EXAMS, params)
        url, body = self.session.api_query.call_args.args
        self.assertTrue(url.endswith(EXAMS))
        self.assertEqual(set(body), {"date", "secretStr"})
        self.assertEqual(json.loads(Cipher(key, iv).decrypt(body["secretStr"])), params)
        self.assertEqual(self.session.api_query.call_args.kwargs["content_type"], "json")

    @unittest.skipUnless(HAS_ZHS, ZH_SKIP)
    def test_missing_success_code_is_not_a_successful_empty_task_list(self):
        self.session.crypto.key_bytes.return_value = b"0123456789abcdef"
        self.session.api_query.return_value = {"data": []}
        with self.assertRaises(RuntimeError):
            ai_query(self.session, TASKS, {})


if __name__ == "__main__":
    unittest.main()
