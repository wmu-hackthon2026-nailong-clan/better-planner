"""AI knowledge-map course discovery and read-only task/progress lists.

Protocol verified against the official student frontend and captured responses.
Never requests exam papers, starts exams, or sends learning/completion records.
"""
import json
import time

AI_ROOT = "https://kg-ai-run.zhihuishu.com/run/gateway/t/"
BASIC = "stu/knowledge-study/course-basic"
TASKS = "stu/task/v2/pc/get-user-tasks"
EXAMS = "stu/task/v2/get-exam-todo-tasks"
READ_PATHS = frozenset((BASIC, TASKS, EXAMS))


def ai_query(session, path, params):
    if path not in READ_PATHS:
        raise ValueError("未允许的 AI 查询接口")
    from zhs.crypto import Cipher
    cipher = Cipher(session.crypto.key_bytes("ai_key"), session.crypto.key_bytes("iv"))
    # This API uses `date` outside the ciphertext, not the legacy dateFormate.
    body = {"date": int(time.time() * 1000),
            "secretStr": cipher.encrypt(json.dumps(params, ensure_ascii=False, separators=(",", ":")))}
    result = session.api_query(AI_ROOT + path, body, content_type="json")
    if not isinstance(result, dict) or result.get("code") != 200:
        raise RuntimeError("AI 接口未成功，请重新登录或稍后重试")
    return result.get("data")


def task_event(row, course, exam=False):
    cid, cls = str(course["courseId"]), str(course["classId"])
    uid = row.get("id")
    title = row.get("taskName") or row.get("examName")
    if uid is None or not title:
        raise ValueError("AI 任务缺少标识或标题")
    task_type = str(row.get("taskType", ""))
    kind = "exam" if exam or task_type == "2" else "homework" if task_type == "1" else "other"
    exam_info = row.get("examTestInfo") or row.get("examInfo") or {}
    old_id = row.get("examId") or exam_info.get("examId") or uid
    # Numeric status is not sufficient evidence of submission/completion.
    return dict(external_id=f"ai:{cid}:{cls}:task:{uid}", kind=kind,
                legacy_ids=[{"external_id": f"ai:{cid}:{prefix}:{old_id}", "kind": "exam"}
                            for prefix in ("todo", "task")],
                title=title, course=course.get("courseName") or cid,
                start=row.get("startTime"), deadline=row.get("endTime") or row.get("endDate"),
                completed=row.get("completed") is True,
                status="进行中" if str(row.get("status")) == "1" else "平台状态 " + str(row.get("status", "未知")),
                url=f"https://ai-smart-course-student-pro.zhihuishu.com/singleCourse/knowledgeStudy/{cid}/{cls}")


def collect_ai(session):
    events, errors, summaries = {}, [], []
    try:
        # Use the strict query instead of upstream get_ai_course_list(), which
        # turns authentication/schema failures into a misleading empty list.
        result = session.zhidao_query(
            session.urls.base + "/gateway/t/v1/student/queryStudentAICourseList",
            {"status": 3}, key=session.crypto.key_bytes("home_key"), ok_code=0)
        ok = isinstance(result, dict) and (
            result.get("code") == 0 if "code" in result else str(result.get("status")) == "200")
        if not ok or not isinstance(result.get("rt"), list):
            raise ValueError("AI 课程列表结构异常")
        courses = result["rt"]
    except Exception:
        return {"events": [], "courses": [], "errors": ["AI 知识图谱课程列表读取失败，请重新登录或检查网络；未计为零门课程"]}
    for course in courses:
        if not isinstance(course, dict) or not course.get("courseId") or not course.get("classId"):
            errors.append("一门 AI 课程缺少 courseId/classId，未读取其任务")
            continue
        params = {"courseId": str(course["courseId"]), "classId": str(course["classId"])}
        summary = dict(name=course.get("courseName") or params["courseId"], system="AI 知识图谱",
                       course_id=params["courseId"], class_id=params["classId"], progress=None,
                       tasks_status="failed", exams_status="failed")
        summaries.append(summary)
        try:
            basic = ai_query(session, BASIC, params)
            if not isinstance(basic, dict) or not isinstance(basic.get("knowledgeStatistics"), dict):
                raise ValueError("缺少进度统计")
            stats = basic["knowledgeStatistics"]
            summary["progress"] = {key: stats.get(key) for key in
                ("knowledgeCount", "knowledgeThemeCount", "subKnowledgeThemeCount", "masteryPercentage", "studyProgress")}
            if basic.get("courseName"):
                course = dict(course, courseName=basic["courseName"])
                summary["name"] = basic["courseName"]
        except Exception:
            errors.append(summary["name"] + "：知识点进度读取失败")
        # Independent requests: an unavailable task list must not hide exams.
        for path, is_exam, field in ((TASKS, False, "tasks_status"), (EXAMS, True, "exams_status")):
            try:
                rows = ai_query(session, path, params)
                if not isinstance(rows, list):
                    raise ValueError("任务列表结构异常")
                converted = [task_event(row, course, is_exam) for row in rows]
                for row in converted:
                    key = row["external_id"]
                    if key not in events or is_exam:
                        events[key] = row
                summary[field] = "success"
            except Exception:
                errors.append(summary["name"] + ("：考试待办读取失败" if is_exam else "：任务列表读取失败"))
        summary["records"] = sum(e["external_id"].startswith(f"ai:{params['courseId']}:{params['classId']}:") for e in events.values())
    return {"events": list(events.values()), "errors": errors, "courses": summaries}
