"""A separate read-only calendar listener; the dashboard stays on loopback."""
from __future__ import annotations

import ipaddress
import secrets
import socket
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

from .core import calendar
from .tunnel import QuickTunnel


def lan_addresses():
    addresses = set()
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.connect(('192.0.2.1', 9))  # routing lookup only; sends no packets
            addresses.add(probe.getsockname()[0])
    except OSError:
        pass
    try:
        addresses.update(item[4][0] for item in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET))
    except OSError:
        pass
    return sorted(a for a in addresses if ipaddress.ip_address(a).is_private
                  and not ipaddress.ip_address(a).is_loopback
                  and not ipaddress.ip_address(a).is_link_local and a != '0.0.0.0')


class Subscription:
    def __init__(self, store):
        self.store = store
        self.path = store.path.parent / 'calendar-subscription.key'
        self.lock = threading.Lock()
        self.server = None
        self.error = None
        self.tunnel = QuickTunnel(store.path.parent.parent)
        if self.path.exists():
            self.key = self.path.read_text(encoding='ascii').strip()
            if len(self.key) < 32:
                raise ValueError('日历订阅密钥文件损坏')
        else:
            self.rotate()

    def rotate(self):
        with self.lock:
            key = secrets.token_urlsafe(32)
            temporary = self.path.with_suffix('.tmp')
            temporary.write_text(key, encoding='ascii')
            temporary.replace(self.path)
            self.key = key

    def state(self):
        port = self.server.server_port if self.server else None
        with self.lock:
            suffix = f'/calendar/{self.key}/calendar.ics'
        relay = self.tunnel.state()
        urls = [f'http://{a}:{port}{suffix}' for a in lan_addresses()] if port else []
        if relay['origin'] and port:
            urls.insert(0, relay['origin'] + suffix)
        return dict(enabled=bool(self.server), port=port, error=self.error, tunnel=relay, urls=urls)

    def start(self, port=8768, host='0.0.0.0'):
        subscription = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass  # the URL contains a bearer credential

            def respond(self, head=False):
                with subscription.lock:
                    expected = f'/calendar/{subscription.key}/calendar.ics'
                valid = secrets.compare_digest(urlsplit(self.path).path.encode(), expected.encode())
                body = calendar(subscription.store.state()['events']).encode() if valid else b'Not found'
                self.send_response(200 if valid else 404)
                self.send_header('Content-Type', 'text/calendar; charset=utf-8' if valid else 'text/plain')
                self.send_header('Content-Length', str(len(body)))
                self.send_header('Cache-Control', 'no-store')
                self.send_header('X-Content-Type-Options', 'nosniff')
                self.end_headers()
                if not head:
                    self.wfile.write(body)

            def do_GET(self):
                self.respond()

            def do_HEAD(self):
                self.respond(head=True)

        try:
            self.server = ThreadingHTTPServer((host, port), Handler)
        except OSError:
            self.error = '订阅端口无法启动，可能已被占用；关闭其他课期实例后重启。'
            return False
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        return True

    def close(self):
        self.tunnel.close()
        if self.server:
            self.server.shutdown()
            self.server.server_close()
            self.server = None
