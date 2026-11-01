"""Run the upstream CLI with all mutable state in this application's data directory."""
import json
import sys
from datetime import date, timedelta
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "data" / "xxt"


def schedule_window(cli, today=None):
    """Expand this and next teaching week from the upstream full-term schedule."""
    from xuexitong_mcp.client import expand_weeks
    today = today or date.today()
    sc = cli._cached_schedule()
    if sc is None:
        client = cli._client()
        cli._login_or_die(client)
        sc = cli._term_schedule(client)
    current = cli._current_week(sc, today) or 1
    term_start = cli._term_start(sc)
    maximum = int(sc.get('curriculum', {}).get('maxWeek') or 30)
    times = cli._lesson_times(sc)
    output = []
    for week in range(current, min(current + 1, maximum) + 1):
        monday = term_start + timedelta(weeks=week - 1) if term_start else today - timedelta(days=today.weekday()) + timedelta(weeks=week - current)
        lessons = [lesson for lesson in sc.get('lessons', [])
                   if (week == current if not lesson.get('weeks') else week in expand_weeks(lesson['weeks']))]
        for _, start, end, name, location, teacher in cli._day_rows(lessons, times, monday):
            output.append(dict(date=f'{start:%Y-%m-%d}', start=f'{start:%H:%M}', end=f'{end:%H:%M}',
                               name=name, location=location, teacher=teacher))
    return output


def configure_parser(parser):
    # Pinned upstream uses default="" but rejects an explicitly empty --course.
    # Preserve that validation while making an omitted filter mean all courses.
    import argparse
    for action in parser._actions:
        if isinstance(action, argparse._SubParsersAction) and "hw" in action.choices:
            action.choices["hw"].set_defaults(course=None)
    return parser


def main():
    from xuexitong_mcp import cli
    from xuexitong_mcp.client import ChaoxingClient
    DATA.mkdir(parents=True, exist_ok=True)
    cli._client = lambda: ChaoxingClient(config_dir=str(DATA))
    cached = cli._cached_schedule
    cli._cached_schedule = lambda max_age_h=None, path=None: cached(max_age_h, path or DATA / "store.json")
    def term_schedule(client):
        sc = cli._cached_schedule()
        if sc is None:
            sc = client.fetch_schedule_full()
            (DATA / "store.json").write_text(json.dumps({"schedule_cache": sc}), encoding="utf-8")
        return sc
    cli._term_schedule = term_schedule
    upstream_week = cli.cmd_week
    def week_command(args):
        if args.week is None and args.json:
            print(json.dumps(schedule_window(cli), ensure_ascii=False))
            return 0
        return upstream_week(args)
    cli.cmd_week = week_command
    build = cli.build_parser
    cli.build_parser = lambda: configure_parser(build())
    return cli.main()


if __name__ == "__main__":
    sys.exit(main())
