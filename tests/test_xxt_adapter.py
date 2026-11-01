import importlib.util
import unittest
from datetime import date, datetime, time
from types import SimpleNamespace

from scripts.xxt_readonly import configure_parser, schedule_window


@unittest.skipUnless(importlib.util.find_spec("xuexitong_mcp"), "Requires installed connector (available in portable runtime)")
class UpstreamParserRegression(unittest.TestCase):
    def test_next_week_uses_actual_teaching_weeks_not_this_week_copy(self):
        schedule = dict(curriculum={'maxWeek': 18}, lessons=[
            {'name': '每周课', 'weeks': '9-10'}, {'name': '单周课', 'weeks': '9'},
            {'name': '仅当前周', 'weeks': ''}])
        def rows(lessons, times, monday):
            return [(1, datetime.combine(monday, time(8)), datetime.combine(monday, time(9)), l['name'], '', '') for l in lessons]
        cli = SimpleNamespace(_cached_schedule=lambda: schedule, _current_week=lambda sc, today: 9,
                              _term_start=lambda sc: date(2026, 8, 31), _lesson_times=lambda sc: {}, _day_rows=rows)
        result = schedule_window(cli, date(2026, 10, 30))
        self.assertEqual({r['name'] for r in result if r['date'] == '2026-10-26'}, {'每周课', '单周课', '仅当前周'})
        self.assertEqual([r['name'] for r in result if r['date'] == '2026-11-02'], ['每周课'])
        schedule['curriculum']['maxWeek'] = 9
        self.assertTrue(all(r['date'] == '2026-10-26' for r in schedule_window(cli, date(2026, 10, 30))))

    def test_omitted_course_does_not_trigger_upstream_empty_filter_error(self):
        from xuexitong_mcp import cli
        parser = configure_parser(cli.build_parser())
        self.assertIsNone(parser.parse_args(["hw", "--all-courses", "--json"]).course)
        self.assertEqual(parser.parse_args(["hw", "--course", ""]).course, "")


if __name__ == "__main__":
    unittest.main()
