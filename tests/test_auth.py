import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from app.auth import AuthManager, crypt


class AuthTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt", "Windows DPAPI")
    def test_dpapi_roundtrip_never_stores_plaintext(self):
        secret = b'{"phone":"test-account","password":"test-password"}'
        encrypted = crypt(secret)
        self.assertNotIn(b"test-password", encrypted)
        self.assertEqual(crypt(encrypted, decrypt=True), secret)

    def test_reject_invalid_login_before_starting_process(self):
        auth = AuthManager()
        with self.assertRaises(ValueError):
            auth.start("chaoxing", {"phone": "", "password": ""}, {})
        with self.assertRaises(ValueError):
            auth.start("chaoxing", {"phone": "x", "password": "p", "remember": "false"}, {})
        with self.assertRaises(ValueError):
            auth.start("unknown", {}, {})

    def test_memory_credentials_do_not_appear_in_status(self):
        auth = AuthManager()
        auth.memory = {"phone": "test-account", "password": "test-password"}
        self.assertEqual(auth.environment()["CHAOXING_PASSWORD"], "test-password")
        self.assertNotIn("test-password", json.dumps(auth.state()))
        self.assertNotIn("test-account", json.dumps(auth.state()))

    @unittest.skipUnless(os.name == "nt", "Windows DPAPI")
    def test_saved_credentials_restore_and_bad_file_is_safe(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "data").mkdir()
            path = root / "data" / "chaoxing.dpapi"
            path.write_bytes(crypt(json.dumps({"phone": "test-user", "password": "test-secret"}).encode()))
            self.assertEqual(AuthManager(root).environment()["CHAOXING_PASSWORD"], "test-secret")
            path.write_bytes(b"corrupt")
            self.assertEqual(AuthManager(root).environment(), {})

    def test_repeated_login_does_not_spawn_duplicate_job(self):
        auth = AuthManager()
        auth.jobs["zhihuishu"] = dict(status="qr", message="waiting")
        with patch("app.auth.subprocess.Popen") as spawn:
            self.assertFalse(auth.start("zhihuishu", {}, {}))
            spawn.assert_not_called()


if __name__ == "__main__":
    unittest.main()
