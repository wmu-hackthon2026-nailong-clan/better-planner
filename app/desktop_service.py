"""Owned desktop login service; stdout carries only calendar snapshots, never credentials."""
from __future__ import annotations

import argparse
import hashlib
import json
import secrets
import threading
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

from .auth import AuthManager
from .connectors import Syncer, config, dependencies
from .core import ROOT, SOURCES, Store


def desktop_events(rows):
    result = []
    undated = 0
    for row in rows:
        if row["source"] not in ("chaoxing", "zhihuishu"):
            continue
        is_class = row["kind"] == "class"
        begin = row["start"] if is_class else row["end"]
        finish = row["end"]
        if not begin or not finish:
            undated += 1
            continue
        result.append(dict(id=row["id"], source=SOURCES[row["source"]],
                           kind="schedule" if is_class else "deadline",
                           start=int(datetime.fromisoformat(begin).timestamp()),
                           end=int(datetime.fromisoformat(finish).timestamp()),
                           title=row["title"], course=row["course"],
                           location=row["location"] or "", teacher=row["teacher"] or "",
                           url=row["url"] or "", done=row["completed"]))
    return result, undated


def make_server(port=0, db_path=None):
    store = Store(db_path)
    auth = AuthManager()
    syncer = Syncer(store, auth)
    token = secrets.token_urlsafe(32)

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def reply(self, value, status=200, mime="application/json; charset=utf-8"):
            data = json.dumps(value, ensure_ascii=False).encode() if not isinstance(value, bytes) else value
            self.send_response(status)
            self.send_header("Content-Type", mime)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Referrer-Policy", "no-referrer")
            self.send_header("Content-Security-Policy", "default-src 'self'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'; form-action 'self'")
            self.end_headers()
            self.wfile.write(data)

        def allowed(self):
            host = f"127.0.0.1:{self.server.server_port}"
            origin = self.headers.get("Origin")
            return (self.headers.get("Host") == host and
                    origin in (None, "http://" + host) and
                    self.headers.get("Sec-Fetch-Site") not in ("cross-site", "same-site"))

        def do_GET(self):
            if not self.allowed():
                return self.reply({"error": "仅允许本机页面访问"}, 403)
            path = urlparse(self.path).path
            if path == "/api/state":
                try:
                    return self.reply(dict(store.state(), token=token, auth=auth.state(),
                                           running=syncer.running, active_provider=syncer.active_provider,
                                           deps=dependencies()))
                except (ValueError, TypeError):
                    return self.reply({"error": "连接器配置无效，请检查 config.local.json"}, 400)
            files = {"/": ("connections.html", "text/html"),
                     "/connections.css": ("connections.css", "text/css"),
                     "/connections.js": ("connections.js", "text/javascript")}
            if path not in files:
                return self.reply({"error": "不存在的路径"}, 404)
            name, mime = files[path]
            return self.reply((ROOT / "web" / name).read_bytes(), mime=mime + "; charset=utf-8")

        def do_POST(self):
            if not self.allowed() or not secrets.compare_digest(self.headers.get("X-Planner-Token", ""), token):
                return self.reply({"error": "请求已失效，请刷新页面"}, 403)
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 8192:
                    raise ValueError("请求内容过大或为空")
                body = json.loads(self.rfile.read(length))
                if not isinstance(body, dict):
                    raise ValueError("请求格式不正确")
                provider = body.get("provider")
                if provider not in ("chaoxing", "zhihuishu"):
                    raise ValueError("请选择学习通或智慧树")
                path = urlparse(self.path).path
                cfg = config()
                report = dependencies(cfg)
                label = SOURCES[provider]
                if any(item["platform"] == label for item in report["missing"]):
                    raise ValueError(label + "连接器未安装，请运行 scripts/setup_desktop.py")
                if path == "/api/auth/login":
                    return self.reply({"started": auth.start(provider, body, cfg)}, 202)
                if path == "/api/sync":
                    if auth.state().get(provider, {}).get("status") in ("running", "qr"):
                        raise ValueError("请先完成登录，再开始同步")
                    return self.reply({"started": syncer.start(provider)}, 202)
                return self.reply({"error": "不存在的接口"}, 404)
            except (ValueError, TypeError, KeyError):
                return self.reply({"error": "请求或配置无效；请确认连接器已安装、平台已选择且登录填写完整。"}, 400)
            except Exception:
                return self.reply({"error": "操作失败，请检查运行环境后重试"}, 500)

    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    server.store, server.auth, server.syncer = store, auth, syncer
    return server


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=0)
    args = parser.parse_args()
    server = make_server(args.port)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print(json.dumps({"type": "ready", "url": f"http://127.0.0.1:{server.server_port}/"}), flush=True)
    last = None
    try:
        while True:
            state = server.store.state()
            events, undated = desktop_events(state["events"])
            payload = dict(type="state", events=events, undated=undated,
                           sync=[{k: v for k, v in row.items() if k != "report"} for row in state["sync"]],
                           running=server.syncer.running)
            serialized = json.dumps(payload, ensure_ascii=False)
            digest = hashlib.sha256(serialized.encode()).digest()
            if digest != last:
                print(serialized, flush=True)
                last = digest
            threading.Event().wait(1)
    except (BrokenPipeError, KeyboardInterrupt):
        pass
    finally:
        server.auth.close()
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
