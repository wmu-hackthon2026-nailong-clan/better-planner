"""Read list endpoints through a separately installed pjm314159/zhs.

Keep stdout as a JSON protocol; upstream logs/prints are redirected to stderr.
No homework/exam context, worker, openExam, doHomework or submit calls.
"""
import contextlib
import json
import sys
import time


class LoginRequired(Exception):
    pass


def has_next_page(value):
    return value is True or (isinstance(value, (str, int)) and str(value).strip().lower() in ("true", "1", "yes"))


def homework_pages(session, cid, rid, flag):
    """Stop on empty pages even when the API returns a truthy string in next."""
    for page in range(100):
        result = session.homework_query(
            f"{session.urls.homework}/studentExam/gateway/t/v1/student/getStudentHomework",
            {"recruitId": rid, "courseId": cid, "flag": flag, "pageSize": 50, "pageNum": page})
        rt = result.get("rt")
        if isinstance(rt, dict) and set(rt) == {"next"} and not has_next_page(rt["next"]):
            return
        if not isinstance(rt, dict) or "studentHomeworkList" not in rt:
            raise RuntimeError("作业列表结构改变或登录失效")
        batch = rt["studentHomeworkList"]
        if not isinstance(batch, list):
            raise RuntimeError("作业列表格式异常")
        if not batch:
            return
        yield from batch
        if not has_next_page(rt.get("next")) and len(batch) < 50:
            return
        time.sleep(0.8)
    raise RuntimeError("作业分页超过限制，结果可能不完整")


def failure_reason(exc):
    name = type(exc).__name__
    if "Timeout" in name:
        return "平台连接超时，请稍后重试"
    if "Connect" in name or "Proxy" in name:
        return "网络连接失败，请检查网络后重试"
    code = getattr(exc, "code", None)
    if code is not None and len(str(code)) < 12:
        return f"平台拒绝读取（状态 {code}），请重新连接账号后重试"
    if isinstance(exc, (KeyError, TypeError, RuntimeError)):
        return "平台返回的列表格式暂不兼容"
    return "读取失败，请稍后重试或重新连接账号"


def collect():
    from zhs.config import ConfigManager
    from zhs.session import ZhsSession
    from zhs.utils.cookie import list_to_cookies
    from zhs.utils.path import get_data_dir
    from zhs.zhidao.course import ZhidaoCourseManager
    from zhs.zhidao.models import extract_course_id

    cfg = ConfigManager().load()
    cookie_path = get_data_dir() / "cookies.json"
    if not cookie_path.exists():
        raise LoginRequired("请在页面连接智慧树")
    session = ZhsSession(cfg)
    events, errors = [], []
    try:
        session.cookies = list_to_cookies(json.loads(cookie_path.read_text(encoding="utf-8")))
        courses = ZhidaoCourseManager(session).get_course_list()
        session.exam_sso_login()
        for c in courses:
            cid = extract_course_id(c)
            rid = str(c.recruit_id or "")
            name = c.course_name or str(cid)
            if not cid or not rid:
                errors.append(f"{name} 缺少课程注册信息")
                continue
            for flag in (1, 2):
                try:
                    for item in homework_pages(session, cid, rid, flag):
                        events.append({"external_id": f"{cid}:hw:{item['id']}", "kind": "homework",
                                       "title": item["examName"], "course": name,
                                       "start": item.get("startTime"), "deadline": item.get("endDate"),
                                       "completed": flag == 2, "url": "https://onlineexamh5new.zhihuishu.com/"})
                except Exception as exc:
                    errors.append(f"{name} {'待交' if flag == 1 else '已交'}作业：{failure_reason(exc)}")
            # Each list is independent; one failure must not skip another list.
            for flag in (1, 0):
                try:
                    batch = session.exam_list(cid, rid, flag=flag)
                    if not isinstance(batch, list):
                        raise RuntimeError("考试列表格式异常")
                    for item in batch:
                        events.append({"external_id": f"{cid}:exam:{item['id']}", "kind": "exam",
                                       "title": item["examName"], "course": name,
                                       "start": item.get("startTime"), "deadline": item.get("endDate"),
                                       "completed": flag == 0, "url": "https://onlineexamh5new.zhihuishu.com/"})
                except Exception as exc:
                    errors.append(f"{name} {'未完成' if flag == 1 else '已完成'}考试：{failure_reason(exc)}")
            time.sleep(0.8)
        return {"events": events, "errors": errors}
    finally:
        session.close()


if __name__ == "__main__":
    try:
        with contextlib.redirect_stdout(sys.stderr):
            result = collect()
        print(json.dumps(result, ensure_ascii=False))
    except LoginRequired:
        print("请先在页面连接智慧树", file=sys.stderr)
        sys.exit(2)
    except ImportError:
        print("请安装智慧树连接器，并在 config.local.json 配置 zhs_python", file=sys.stderr)
        sys.exit(4)
    except Exception:
        print("智慧树读取失败，请先运行 zhs login，检查依赖版本和登录状态", file=sys.stderr)
        sys.exit(1)
