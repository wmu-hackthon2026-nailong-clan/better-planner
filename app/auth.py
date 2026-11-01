"""Local login jobs and Windows user-bound credential encryption."""
from __future__ import annotations

import base64
import atexit
import ctypes
import json
import os
import subprocess
import sys
import threading
from ctypes import wintypes

from .core import ROOT


def crypt(data, decrypt=False):
    if os.name != "nt":
        raise RuntimeError("记住密码仅支持 Windows；其他系统请使用本次登录")

    class Blob(ctypes.Structure):
        _fields_ = [("size", wintypes.DWORD), ("data", ctypes.POINTER(ctypes.c_byte))]

    buffer = ctypes.create_string_buffer(data)
    incoming = Blob(len(data), ctypes.cast(buffer, ctypes.POINTER(ctypes.c_byte)))
    outgoing = Blob()
    api = ctypes.WinDLL("crypt32", use_last_error=True)
    fn = api.CryptUnprotectData if decrypt else api.CryptProtectData
    fn.argtypes = [ctypes.POINTER(Blob), ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                   ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(Blob)]
    fn.restype = wintypes.BOOL
    if not fn(ctypes.byref(incoming), None, None, None, None, 1, ctypes.byref(outgoing)):
        raise RuntimeError("Windows 登录凭据加密失败，请使用本次登录")
    try:
        return ctypes.string_at(outgoing.data, outgoing.size)
    finally:
        free = ctypes.WinDLL("kernel32").LocalFree
        free.argtypes = [ctypes.c_void_p]
        free.restype = ctypes.c_void_p
        free(outgoing.data)


class AuthManager:
    def __init__(self, root=ROOT):
        self.root = root
        self.lock = threading.Lock()
        self.jobs = {}
        self.memory = {}
        self.processes = {}
        atexit.register(self.close)

    def close(self):
        with self.lock:
            processes = list(self.processes.values())
        for process in processes:
            if process.poll() is None:
                process.kill()

    def environment(self):
        creds = dict(self.memory)
        path = self.root / "data" / "chaoxing.dpapi"
        if not creds and path.exists():
            try:
                creds = json.loads(crypt(path.read_bytes(), decrypt=True))
            except Exception:
                pass
        return {"CHAOXING_PHONE": creds["phone"], "CHAOXING_PASSWORD": creds["password"]} if creds else {}

    def state(self):
        with self.lock:
            return {k: dict(v) for k, v in self.jobs.items()}

    def start(self, provider, body, cfg):
        if provider not in ("chaoxing", "zhihuishu"):
            raise ValueError("不支持的平台")
        if "remember" in body and not isinstance(body["remember"], bool):
            raise ValueError("记住登录必须是布尔值")
        if provider == "chaoxing":
            if not isinstance(body.get("phone"), str) or not isinstance(body.get("password"), str):
                raise ValueError("请输入学习通账号和密码")
            if not body["phone"].strip() or not body["password"] or len(body["password"]) > 500:
                raise ValueError("账号或密码格式不正确")
        with self.lock:
            if self.jobs.get(provider, {}).get("status") in ("running", "qr"):
                return False
            self.jobs[provider] = {"status": "running", "message": "正在连接，请稍候…"}
        threading.Thread(target=self._run, args=(provider, dict(body), cfg), daemon=True).start()
        return True

    def _run(self, provider, body, cfg):
        process = None
        timer = None
        try:
            bundled = ROOT / "runtime" / "python.exe"
            python = cfg.get("auth_python") or cfg.get("zhs_python") or (str(bundled) if bundled.exists() else sys.executable)
            # Dedicated auth interpreter may be necessary when connectors use separate environments.
            if provider == "chaoxing" and cfg.get("xxt_command", [""])[0].lower().endswith("python.exe"):
                python = cfg["xxt_command"][0]
            env = dict(os.environ, PYTHONIOENCODING="utf-8", NO_COLOR="1", XXT_HOME=str(ROOT / "data" / "xxt"))
            workdir = ROOT / "data" / ("zhs" if provider == "zhihuishu" else "xxt")
            workdir.mkdir(parents=True, exist_ok=True)
            process = subprocess.Popen([python, str(ROOT / "scripts" / "auth_worker.py"), provider],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                       encoding="utf-8", env=env, cwd=str(workdir),
                                       creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            with self.lock:
                self.processes[provider] = process
            timer = threading.Timer(180, process.kill)
            timer.daemon = True
            timer.start()
            process.stdin.write(json.dumps(body, ensure_ascii=False) + "\n")
            process.stdin.close()
            succeeded = False
            for line in process.stdout:
                result = json.loads(line)
                if result.get("status") == "qr":
                    raw = base64.b64decode(result["image"], validate=True)
                    if len(raw) > 1_000_000:
                        raise ValueError("二维码过大")
                with self.lock:
                    self.jobs[provider] = result
                succeeded = result.get("status") == "success"
            process.wait()
            if process.returncode or not succeeded:
                with self.lock:
                    if self.jobs[provider].get("status") != "error":
                        self.jobs[provider] = {"status": "error", "message": "登录未完成或已超时，请重试。若需验证码，请到官方客户端完成验证后重试。"}
                return
            if provider == "chaoxing":
                self.memory = {"phone": body["phone"], "password": body["password"]}
                path = self.root / "data" / "chaoxing.dpapi"
                if body.get("remember"):
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(crypt(json.dumps(self.memory).encode()))
                elif path.exists():
                    path.unlink()
        except FileNotFoundError:
            with self.lock:
                self.jobs[provider] = {"status": "error", "message": "连接器未安装；请使用包含运行环境的完整启动包。"}
        except Exception:
            with self.lock:
                self.jobs[provider] = {"status": "error", "message": "连接失败，请检查连接器安装、网络和账号状态后重试。"}
        finally:
            body.clear()
            if timer:
                timer.cancel()
            if process:
                if process.poll() is None:
                    process.kill()
                process.wait()
                if process.stdout:
                    process.stdout.close()
            with self.lock:
                self.processes.pop(provider, None)
