from __future__ import annotations

import hashlib
import json
import sqlite3
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
from pathlib import Path

TZ = timezone(timedelta(hours=8))
ROOT = Path(__file__).resolve().parent.parent
SOURCES = {"chaoxing": "学习通", "zhihuishu": "智慧树", "manual": "手动"}


def now():
    return datetime.now(TZ)


def date_value(value):
    if value is None or value == "" or value in ("未知", "截止未知", "不限", "无"):
        return None
    if isinstance(value, (float, int)) or str(value).isdigit():
        n = float(value)
        return datetime.fromtimestamp(n / 1000 if n > 1e11 else n, TZ).isoformat(timespec="seconds")
    text = str(value).strip().replace("/", "-").replace("Z", "+00:00")
    try:
        d = datetime.fromisoformat(text)
    except ValueError as exc:
        raise ValueError(f"无法解析日期：{value}，请使用 YYYY-MM-DD HH:MM") from exc
    return (d.replace(tzinfo=TZ) if d.tzinfo is None else d.astimezone(TZ)).isoformat(timespec="seconds")


def normalize(row, source, kind=None):
    if source not in SOURCES:
        raise ValueError("未知来源")
    if not isinstance(row, dict):
        raise ValueError("每条记录必须是对象")
    kind = kind or row.get("kind", "homework")
    if kind not in ("class", "homework", "exam", "other"):
        raise ValueError("不支持的任务类型")
    title = str(row.get("title") or row.get("name") or "").strip()
    if not title or len(title) > 300:
        raise ValueError("标题需为 1–300 个字符")
    course = str(row.get("course") or (title if kind == "class" else ""))[:300]
    start = row.get("start") or row.get("startTime")
    end = row.get("end") or row.get("deadline") or row.get("endDate")
    if kind == "class" and row.get("date"):
        start = f"{row['date']} {start}"
        end = f"{row['date']} {end}"
    start, end = date_value(start), date_value(end)
    if kind == "class" and (not start or not end):
        raise ValueError("课程必须有开始和结束时间")
    if start and end and start > end:
        raise ValueError("结束时间不能早于开始时间")
    raw_status = str(row.get("status") or row.get("answer") or "")
    done = row.get("completed") is True or raw_status in ("done", "已完成", "已提交", "待批阅", "已批阅", "已批改", "已交", "已考")
    external_id = row.get("external_id") or row.get("id")
    # Deadline changes must update an existing task, rather than create another one.
    identity = str(external_id) if external_id is not None else "|".join((course, title, start or ""))
    uid = hashlib.sha256(f"{source}|{kind}|{identity}".encode()).hexdigest()[:24]
    url = str(row.get("url") or "")
    if not url.startswith(("https://", "http://")):
        url = ""
    return dict(id=uid, source=source, kind=kind, title=title, course=course,
                start=start, end=end, location=str(row.get("location") or "")[:300],
                teacher=str(row.get("teacher") or "")[:100], url=url,
                platform_done=int(done), status=raw_status[:100])


class Store:
    def __init__(self, path=None):
        self.path = Path(path or ROOT / "data" / "planner.sqlite3")
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.connect() as db:
            db.executescript('''
                CREATE TABLE IF NOT EXISTS events(
                    id TEXT PRIMARY KEY, source TEXT NOT NULL, kind TEXT NOT NULL,
                    title TEXT NOT NULL, course TEXT NOT NULL, start TEXT, end TEXT,
                    location TEXT, teacher TEXT, url TEXT, platform_done INTEGER,
                    status TEXT, local_done INTEGER DEFAULT 0, updated_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS sync_state(
                    stream TEXT PRIMARY KEY, success_at TEXT, attempted_at TEXT, error TEXT
                );
            ''')

    @contextmanager
    def connect(self):
        db = sqlite3.connect(self.path, timeout=10)
        db.row_factory = sqlite3.Row
        try:
            with db:
                yield db
        finally:
            db.close()

    def merge(self, rows):
        stamp = now().isoformat(timespec="seconds")
        with self.connect() as db:
            for row in rows:
                values = dict(row, updated_at=stamp)
                keys = list(values)
                db.execute(f"INSERT INTO events({','.join(keys)}) VALUES({','.join(':'+k for k in keys)}) "
                           f"ON CONFLICT(id) DO UPDATE SET {','.join(k+'=excluded.'+k for k in keys if k != 'id')}", values)

    def record_sync(self, stream, error=None):
        stamp = now().isoformat(timespec="seconds")
        with self.connect() as db:
            db.execute('''INSERT INTO sync_state VALUES(?,?,?,?) ON CONFLICT(stream) DO UPDATE SET
                attempted_at=excluded.attempted_at, error=excluded.error,
                success_at=COALESCE(excluded.success_at, sync_state.success_at)''',
                       (stream, None if error else stamp, stamp, error))

    def state(self):
        with self.connect() as db:
            rows = [dict(r) for r in db.execute("SELECT * FROM events ORDER BY COALESCE(end,start,'9999'),title")]
            sync = [dict(r) for r in db.execute("SELECT * FROM sync_state ORDER BY stream")]
        for r in rows:
            r["completed"] = bool(r["local_done"] or r["platform_done"])
        return dict(events=rows, sync=sync, now=now().isoformat(), timezone="Asia/Shanghai")

    def complete(self, uid, done):
        with self.connect() as db:
            cur = db.execute("UPDATE events SET local_done=? WHERE id=? AND kind!='class'", (int(done), uid))
            if not cur.rowcount:
                raise ValueError("任务不存在")

    def delete_manual(self, uid):
        with self.connect() as db:
            cur = db.execute("DELETE FROM events WHERE id=? AND source='manual'", (uid,))
            if not cur.rowcount:
                raise ValueError("只能删除手动添加的记录")


def calendar(events):
    def esc(text):
        return str(text).replace("\\", "\\\\").replace("\r", "").replace("\n", "\\n").replace(";", "\\;").replace(",", "\\,")

    def utc(value):
        return datetime.fromisoformat(value).astimezone(timezone.utc).strftime("%Y%m%dT%H%M%SZ")

    lines = ["BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//Keqi//Planner//ZH", "CALSCALE:GREGORIAN", "X-WR-CALNAME:课期 · 课表与截止日期"]
    for e in events:
        if e["completed"] or not (e["end"] or e["start"]):
            continue
        is_class = e['kind'] == 'class'
        if not is_class and not e['end']:
            continue
        lines += ["BEGIN:VEVENT", f"UID:{e['id']}@keqi.local", "DTSTAMP:" + utc(now().isoformat()),
                  "LAST-MODIFIED:" + utc(e.get('updated_at') or now().isoformat())]
        description = SOURCES[e['source']] + ' · ' + e['course']
        if is_class:
            start, end = e['start'] or e['end'], e['end'] or e['start']
            lines.append('DTSTART:' + utc(start))
            if end > start:
                lines.append('DTEND:' + utc(end))
        else:
            deadline = datetime.fromisoformat(e['end']).astimezone(TZ)
            # DATE DTEND is exclusive: this is exactly one local calendar day.
            day = deadline.date() - timedelta(days=1)
            lines += ['DTSTART;VALUE=DATE:' + day.strftime('%Y%m%d'),
                      'DTEND;VALUE=DATE:' + deadline.date().strftime('%Y%m%d')]
            description += '\n实际截止：' + deadline.strftime('%Y-%m-%d %H:%M') + '（北京时间）'
        lines += [
                  "SUMMARY:" + esc(("明日截止 · " if not is_class else "") + e["title"]),
                  "DESCRIPTION:" + esc(description),
                  "LOCATION:" + esc(e["location"] or ""), "END:VEVENT"]
    lines.append("END:VCALENDAR")
    # RFC 5545: fold at <=75 octets, never split a UTF-8 character.
    folded = []
    for line in lines:
        chunk = ""
        for ch in line:
            if len((chunk + ch).encode()) > 75:
                folded.append(chunk)
                chunk = " "
            chunk += ch
        folded.append(chunk)
    return "\r\n".join(folded) + "\r\n"
