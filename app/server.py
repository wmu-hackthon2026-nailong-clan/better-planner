from __future__ import annotations

import argparse
import json
import secrets
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

from .connectors import Syncer, config, dependencies
from .core import ROOT, Store, calendar, normalize
from .auth import AuthManager
from .subscription import Subscription


def make_server(port=8765, db_path=None):
    store = Store(db_path)
    auth = AuthManager()
    syncer = Syncer(store, auth)
    token = secrets.token_urlsafe(32)
    subscription = Subscription(store)

    class Handler(BaseHTTPRequestHandler):
        def reply(self, value, status=200, content_type="application/json; charset=utf-8"):
            body = json.dumps(value, ensure_ascii=False).encode() if isinstance(value, (dict, list)) else value
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Content-Security-Policy", "default-src 'self'; style-src 'self'; script-src 'self'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'; form-action 'self'")
            self.end_headers()
            self.wfile.write(body)

        def allowed_host(self):
            return self.headers.get("Host") in (f"127.0.0.1:{self.server.server_port}", f"localhost:{self.server.server_port}")

        def do_GET(self):
            if not self.allowed_host():
                return self.reply({"error": "仅允许本机访问"}, 403)
            path = urlparse(self.path).path
            if path == "/api/state":
                return self.reply(dict(store.state(), app="keqi", running=syncer.running, token=token,
                                       auth=auth.state(), subscription=subscription.state(),
                                       interval=config()["sync_interval_minutes"], deps=dependencies()))
            if path == "/api/calendar.ics":
                return self.reply(calendar(store.state()["events"]).encode(), content_type="text/calendar; charset=utf-8")
            files = {"/": ("index.html", "text/html"), "/style.css": ("style.css", "text/css"),
                     "/app.js": ("app.js", "text/javascript")}
            if path not in files:
                return self.reply({"error": "不存在的路径"}, 404)
            name, mime = files[path]
            return self.reply((ROOT / "web" / name).read_bytes(), content_type=mime + "; charset=utf-8")

        def do_POST(self):
            if not self.allowed_host() or self.headers.get("X-Planner-Token") != token:
                return self.reply({"error": "请求校验失败，请刷新页面"}, 403)
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 2_000_000:
                    raise ValueError("请求大小需在 1 字节至 2 MB 之间")
                body = json.loads(self.rfile.read(length))
                if not isinstance(body, dict):
                    raise ValueError("请求必须为 JSON 对象")
                path = urlparse(self.path).path
                if path == "/api/auth/login":
                    return self.reply({"started": auth.start(body.get("provider"), body, config())}, 202)
                if path == "/api/subscription/reset":
                    subscription.rotate()
                    return self.reply({"ok": True})
                if path == "/api/subscription/https":
                    if not isinstance(body.get("enabled"), bool):
                        raise ValueError("enabled 必须为布尔值")
                    if body['enabled']:
                        if not subscription.server:
                            raise ValueError('请先启动日历订阅服务')
                        return self.reply({"started": subscription.tunnel.start(subscription.server.server_port)}, 202)
                    subscription.tunnel.close()
                    return self.reply({"ok": True})
                if path == "/api/sync":
                    return self.reply({"started": syncer.start()}, 202)
                if path == "/api/events":
                    store.merge([normalize(body, "manual")])
                elif path == "/api/import":
                    source = body.get("source")
                    items = body.get("events")
                    if source not in ("chaoxing", "zhihuishu") or not isinstance(items, list) or len(items) > 5000:
                        raise ValueError("请选择平台并提供不超过 5000 条的 events 数组")
                    rows = [normalize(r, source) for r in items]
                    store.merge(rows)
                    store.record_sync(("学习通" if source == "chaoxing" else "智慧树") + "文件导入")
                elif path == "/api/complete":
                    if not isinstance(body.get("done"), bool):
                        raise ValueError("done 必须是布尔值")
                    store.complete(body["id"], body["done"])
                elif path == "/api/delete":
                    store.delete_manual(body["id"])
                else:
                    return self.reply({"error": "不存在的接口"}, 404)
                self.reply({"ok": True})
            except (ValueError, KeyError, TypeError) as exc:
                self.reply({"error": str(exc)}, 400)
            except Exception:
                self.reply({"error": "本地操作失败，请查看终端"}, 500)

    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    server.store, server.syncer = store, syncer
    server.auth = auth
    server.subscription = subscription
    return server


def main():
    parser = argparse.ArgumentParser(description="课期 · 本地课表与截止日期汇总")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--auto-sync", action="store_true", help="启动后同步，之后按配置间隔同步")
    parser.add_argument("--calendar-port", type=int, default=8768, help="只读日历订阅端口，默认 8768")
    args = parser.parse_args()
    server = make_server(args.port)
    server.subscription.start(args.calendar_port)
    stop = threading.Event()
    if args.auto_sync:
        def poll():
            while not stop.is_set():
                server.syncer.start()
                if stop.wait(config()["sync_interval_minutes"] * 60):
                    return
        threading.Thread(target=poll, daemon=True).start()
    print(f"课期已启动：http://127.0.0.1:{server.server_port}  （Ctrl+C 退出）", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        server.subscription.close()
        server.server_close()


if __name__ == "__main__":
    main()
