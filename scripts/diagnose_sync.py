"""Read-only troubleshooting. Output counts and error categories, never credentials or URLs."""
import contextlib
import importlib.util
import json
import sys
from pathlib import Path


def report(**data):
    print(json.dumps(data, ensure_ascii=True), flush=True)


def error_info(exc):
    code = getattr(exc, "code", None)
    response = getattr(exc, "response", None)
    msg = str(exc)
    return dict(type=type(exc).__name__, code=code if isinstance(code, (int, str)) and len(str(code)) < 20 else None,
                http=getattr(response, "status_code", None),
                category="missing_work_signature" if "workEnc" in msg else
                "permission" if "权限" in msg else "other")


def xxt(root):
    from xuexitong_mcp.client import ChaoxingClient
    client = ChaoxingClient(config_dir=str(root / "data" / "xxt"))
    original = client.fetch_homework
    def fetch(course, *args, **kw):
        try:
            result = original(course, *args, **kw)
            report(platform="xxt", course=course["name"], items=len(result["items"]))
            return result
        except Exception as exc:
            report(platform="xxt", course=course["name"], **error_info(exc))
            raise
    client.fetch_homework = fetch
    failed = []
    rows = client.fetch_deadline_overview(errors=failed)
    report(platform="xxt", returned=len(rows), failed=len(failed))


def zhs(root):
    from loguru import logger
    logger.remove()
    from zhs.session import ZhsSession
    for method in ("homework_query", "exam_list"):
        original = getattr(ZhsSession, method)
        def wrapper(self, *args, _original=original, _method=method, **kw):
            try:
                result = _original(self, *args, **kw)
                if _method == "homework_query":
                    rt = result.get("rt")
                    report(platform="zhs", method=_method, flag=args[1].get("flag"),
                           rt_type=type(rt).__name__, keys=list(rt) if isinstance(rt, dict) else [],
                           count=len(rt.get("studentHomeworkList") or []) if isinstance(rt, dict) else None)
                else:
                    report(platform="zhs", method=_method, count=len(result) if isinstance(result, list) else None,
                           result_type=type(result).__name__)
                return result
            except Exception as exc:
                report(platform="zhs", method=_method, **error_info(exc))
                raise
        setattr(ZhsSession, method, wrapper)
    spec = importlib.util.spec_from_file_location("bridge", root / "scripts" / "zhs_readonly.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    result = module.collect()
    report(platform="zhs", returned=len(result["events"]), errors=result["errors"],
           completed=sum(bool(e.get("completed")) for e in result["events"]))


if __name__ == "__main__":
    root = Path(sys.argv[2]).resolve()
    try:
        (xxt if sys.argv[1] == "xxt" else zhs)(root)
    except Exception as exc:
        report(fatal=True, **error_info(exc))
