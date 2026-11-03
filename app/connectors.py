"""Query adapters. No platform submission or completion APIs are invoked."""
from __future__ import annotations

import importlib.util
import json
import os
import shutil
import subprocess
import sys
import threading
from pathlib import Path

from .core import ROOT, normalize


def config():
    path = ROOT / "config.local.json"
    data = json.loads(path.read_text(encoding="utf-8-sig")) if path.exists() else {}
    interval = int(data.get("sync_interval_minutes", 30))
    data["sync_interval_minutes"] = max(15, interval)
    portable = ROOT / "runtime" / "python.exe"
    if portable.exists():
        data.setdefault("xxt_command", [str(portable), str(ROOT / "scripts" / "xxt_readonly.py")])
        data.setdefault("zhs_python", str(portable))
        data.setdefault("xxt_cwd", str(ROOT))
    return data


def _command_exists(value):
    return bool(value) and (shutil.which(str(value)) is not None or Path(str(value)).exists())


def dependencies(cfg=None):
    """Pre-flight check for the two connectors, surfaced before the user clicks sync.

    Returns a structured report the UI can render as an actionable banner instead of
    a generic post-failure error. Login state is intentionally not inspected here:
    a missing environment is a different problem from an expired session.
    """
    cfg = cfg if cfg is not None else config()
    missing = []
    xxt = ((cfg.get("xxt_command") or ["xxt"])[0]) if cfg.get("xxt_command") else "xxt"
    if not _command_exists(xxt):
        missing.append({"platform": "学习通", "path": str(xxt)})
    zhs = cfg.get("zhs_python")
    zhs_ok = _command_exists(zhs) if zhs else importlib.util.find_spec("zhs") is not None
    if not zhs_ok:
        missing.append({"platform": "智慧树", "path": str(zhs or sys.executable)})
    return {
        "ready": not missing,
        "missing": missing,
        "hint": "运行 scripts\\install-connectors.ps1 安装连接器（需要 Git 与网络），或使用完整启动包。" if missing else "",
    }


def run_json(command, cwd=None, timeout=240, extra_env=None):
    env = dict(os.environ, PYTHONIOENCODING="utf-8", NO_COLOR="1", XXT_HOME=str(ROOT / "data" / "xxt"))
    env.update(extra_env or {})
    try:
        result = subprocess.run(command, cwd=cwd, capture_output=True, encoding="utf-8", errors="replace",
                                timeout=timeout, env=env, shell=False,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except FileNotFoundError as exc:
        raise RuntimeError("平台连接组件未就绪，请使用完整的 Better Planner 启动包") from exc
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError("查询超时；保留上次数据，请稍后重试") from exc
    if result.returncode:
        if result.returncode == 2:
            raise RuntimeError("尚未登录或登录已失效，请点击「连接账号」完成登录")
        if result.returncode == 3:
            raise RuntimeError("平台要求人机验证，请在官方客户端完成验证后重试")
        raise RuntimeError("查询失败，请检查网络或重新连接账号后重试")
    try:
        value = json.loads(result.stdout.lstrip("\ufeff"))
    except json.JSONDecodeError as exc:
        raise RuntimeError("连接器没有返回有效 JSON；请检查版本及登录状态") from exc
    # xxt can exit successfully despite individual courses failing. Never label that a full success.
    if result.stderr.strip() and "没拉到" in result.stderr:
        if not isinstance(value, list):
            raise RuntimeError("部分课程查询失败，返回格式异常")
        return {"events": value, "errors": ["部分课程未开放作业或查询失败；已保存其他课程成功返回的作业"]}
    return value


class Syncer:
    def __init__(self, store, auth=None):
        self.store = store
        self.auth = auth
        self.lock = threading.Lock()
        self.running = False
        self.active_provider = None

    def start(self, provider=None):
        if provider not in (None, "chaoxing", "zhihuishu"):
            raise ValueError("不支持的平台")
        if not self.lock.acquire(blocking=False):
            return False
        self.running = True
        self.active_provider = provider
        threading.Thread(target=self._run, args=(provider,), daemon=True).start()
        return True

    def _run(self, provider=None):
        try:
            cfg = config()
            cmd = cfg.get("xxt_command") or ["xxt"]
            if not isinstance(cmd, list) or not all(isinstance(s, str) for s in cmd):
                raise ValueError("xxt_command 必须是字符串数组")
            for stream, args, kind in [("学习通课表", ["week", "--json"], "class"),
                                       ("学习通作业", ["hw", "--all-courses", "--json"], "homework"),
                                       ("学习通考试", ["exam", "--json"], "exam")]:
                if provider == "zhihuishu":
                    continue
                try:
                    data = run_json(cmd + args, cfg.get("xxt_cwd"), extra_env=self.auth.environment() if self.auth else {})
                    warnings = []
                    if kind == "homework" and isinstance(data, dict):
                        warnings = data.get("errors", [])
                        data = data.get("events")
                    if not isinstance(data, list):
                        raise ValueError("连接器返回格式异常，应为列表")
                    rows = []
                    for row in data:
                        if kind == "exam":
                            row = dict(row, status=row.get("answer") or row.get("status") or "")
                        rows.append(normalize(row, "chaoxing", kind))
                    report = self.store.merge(rows)
                    report["status"] = "partial" if warnings else "success"
                    self.store.record_sync(stream, "；".join(warnings) or None, report)
                except Exception as exc:
                    self.store.record_sync(stream, str(exc))
            if provider == "chaoxing":
                return
            try:
                workdir = ROOT / "data" / "zhs"
                workdir.mkdir(parents=True, exist_ok=True)
                data = run_json([cfg.get("zhs_python") or sys.executable, str(ROOT / "scripts" / "zhs_readonly.py")], cwd=str(workdir), timeout=600)
                if not isinstance(data, dict) or not isinstance(data.get("events"), list):
                    raise ValueError("智慧树连接器格式异常")
                rows = [normalize(r, "zhihuishu") for r in data["events"]]
                aliases = {row["id"]: [normalize(dict(raw, external_id=old["external_id"], kind=old["kind"]), "zhihuishu")["id"]
                           for old in raw.get("legacy_ids", [])] for raw, row in zip(data["events"], rows)}
                report = self.store.merge(rows, aliases=aliases)
                report["courses"] = data.get("courses", [])
                report["status"] = "partial" if data.get("errors") else "success"
                self.store.record_sync("智慧树任务", "；".join(data.get("errors", [])) or None, report)
            except Exception as exc:
                self.store.record_sync("智慧树任务", str(exc))
        except Exception as exc:
            self.store.record_sync("同步配置", str(exc))
            streams = (["学习通课表", "学习通作业", "学习通考试"] if provider != "zhihuishu" else [])
            if provider != "chaoxing":
                streams.append("智慧树任务")
            for stream in streams:
                self.store.record_sync(stream, str(exc))
        finally:
            self.running = False
            self.active_provider = None
            self.lock.release()
