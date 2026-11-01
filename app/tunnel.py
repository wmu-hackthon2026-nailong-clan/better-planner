"""Optional, temporary HTTPS relay for the read-only calendar listener."""
from __future__ import annotations

import atexit
import re
import subprocess
import threading


def tunnel_origin(line):
    match = re.search(r'https://[a-z0-9-]+\.trycloudflare\.com(?=[\s/|]|$)', line)
    return match.group(0) if match else None


class QuickTunnel:
    def __init__(self, root):
        self.root = root
        self.lock = threading.Lock()
        self.process = None
        self.status = 'off'
        self.origin = None
        self.message = '尚未启用 HTTPS 临时订阅'
        atexit.register(self.close)

    def state(self):
        with self.lock:
            return dict(status=self.status, origin=self.origin if self.status == 'ready' else None,
                        message=self.message)

    def start(self, port):
        with self.lock:
            if self.status in ('starting', 'ready', 'reconnecting'):
                return False
            executable = self.root / 'tools' / 'cloudflared.exe'
            if not executable.exists():
                executable = self.root / '.deps' / 'cloudflared' / 'cloudflared-windows-amd64.exe'
            if not executable.exists():
                raise ValueError('缺少 HTTPS 组件，请使用最新版完整便携包')
            self.status, self.origin = 'starting', None
            self.message = '正在建立 HTTPS 通道，可能需要约一分钟…'
            try:
                self.process = subprocess.Popen([
                    str(executable), 'tunnel', '--url', f'http://127.0.0.1:{port}',
                    '--protocol', 'http2', '--no-autoupdate'],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    encoding='utf-8', errors='replace',
                    creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            except OSError:
                self.status = 'error'
                self.message = '无法启动 HTTPS 组件，请检查完整启动包'
                raise ValueError(self.message) from None
            process = self.process
        threading.Thread(target=self._read, args=(process,), daemon=True).start()
        return True

    def _read(self, process):
        def timeout():
            with self.lock:
                if self.process is process and self.status == 'starting':
                    self.status, self.origin = 'error', None
                    self.message = 'HTTPS 通道建立超时，请检查网络后重试'
                    process.terminate()
        timer = threading.Timer(90, timeout)
        timer.daemon = True
        timer.start()
        try:
            for line in process.stdout:
                with self.lock:
                    if self.process is not process:
                        break
                    origin = tunnel_origin(line)
                    if origin:
                        self.origin = origin
                    if 'Registered tunnel connection' in line and self.origin:
                        self.status = 'ready'
                        self.message = 'HTTPS 通道已连接；请将 HTTPS 链接粘贴到手机日历的 URL 导入'
                        timer.cancel()
                    elif 'Unable to establish connection' in line and self.status == 'ready':
                        self.status = 'reconnecting'
                        self.message = 'HTTPS 通道正在重新连接，请稍候'
            process.wait()
        finally:
            timer.cancel()
            process.stdout.close()
            with self.lock:
                if self.process is process:
                    self.process = None
                    self.origin = None
                    if self.status != 'error':
                        self.status = 'error'
                        self.message = 'HTTPS 通道已断开，请重新建立'

    def close(self):
        with self.lock:
            process, self.process = self.process, None
            self.status, self.origin = 'off', None
            self.message = 'HTTPS 通道已关闭'
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
