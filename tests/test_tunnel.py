import io
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import patch

from app.tunnel import QuickTunnel, tunnel_origin


class FakeProcess:
    def __init__(self):
        self.stdout = io.StringIO('INF https://example-calendar.trycloudflare.com |\nINF Registered tunnel connection\n')
        self.read = threading.Event()
        self.stopped = threading.Event()

    def wait(self, timeout=None):
        self.read.set()
        self.stopped.wait(timeout)
        return 0

    def poll(self):
        return 0 if self.stopped.is_set() else None

    def terminate(self):
        self.stopped.set()


class TunnelTests(unittest.TestCase):
    def test_accepts_only_official_quick_tunnel_origins(self):
        self.assertEqual(tunnel_origin('INF | https://a-b.trycloudflare.com |'), 'https://a-b.trycloudflare.com')
        self.assertIsNone(tunnel_origin('https://a.trycloudflare.com.evil.test'))
        self.assertIsNone(tunnel_origin('http://a.trycloudflare.com'))

    def test_relay_targets_only_calendar_port_and_stops_owned_process(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'tools').mkdir()
            (root / 'tools' / 'cloudflared.exe').touch()
            relay = QuickTunnel(root)
            process = FakeProcess()
            with patch('app.tunnel.subprocess.Popen', return_value=process) as spawn:
                self.assertTrue(relay.start(8768))
                self.assertTrue(process.read.wait(2))
                self.assertEqual(relay.state()['status'], 'ready')
                self.assertEqual(relay.state()['origin'], 'https://example-calendar.trycloudflare.com')
                self.assertFalse(relay.start(8768))
                self.assertIn('http://127.0.0.1:8768', spawn.call_args.args[0])
                relay.close()
                self.assertTrue(process.stopped.is_set())
                self.assertEqual(relay.state()['status'], 'off')
                self.assertIsNone(relay.state()['origin'])


if __name__ == '__main__':
    unittest.main()
