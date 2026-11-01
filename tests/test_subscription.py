import tempfile
import unittest
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from app.core import Store, normalize
from app.subscription import Subscription


class SubscriptionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.store = Store(Path(self.temp.name) / 'planner.sqlite3')
        self.feed = Subscription(self.store)
        self.assertTrue(self.feed.start(0, '127.0.0.1'))
        self.base = f'http://127.0.0.1:{self.feed.server.server_port}'

    def tearDown(self):
        self.feed.close()
        self.temp.cleanup()

    def url(self):
        return f'{self.base}/calendar/{self.feed.key}/calendar.ics'

    def test_feed_contains_classes_deadlines_and_live_updates(self):
        self.store.merge([
            normalize(dict(title='数学课', kind='class', start='2026-11-01 08:00', end='2026-11-01 09:40'), 'chaoxing'),
            normalize(dict(external_id='1', title='章节作业', end='2026-11-02 20:00'), 'zhihuishu'),
            normalize(dict(title='已交作业', completed=True, end='2026-11-02 21:00'), 'chaoxing'),
        ])
        with urlopen(self.url()) as response:
            text = response.read().decode()
            self.assertIn('text/calendar', response.headers['Content-Type'])
        self.assertIn('数学课', text)
        self.assertIn('截止 · 章节作业', text)
        self.assertNotIn('已交作业', text)
        self.store.merge([normalize(dict(external_id='1', title='章节作业', end='2026-11-03 20:00'), 'zhihuishu')])
        with urlopen(self.url()) as response:
            text = response.read().decode()
        self.assertIn('DTSTART;VALUE=DATE:20261102', text)
        self.assertIn('DTEND;VALUE=DATE:20261103', text)
        self.assertNotIn('DTSTART;VALUE=DATE:20261101', text)
        with urlopen(Request(self.url(), method='HEAD')) as response:
            self.assertEqual(response.status, 200)
            self.assertEqual(response.read(), b'')

    def test_only_secret_feed_route_is_available(self):
        for path in ('/api/state', '/', '/api/calendar.ics', '/calendar/wrong/calendar.ics'):
            with self.assertRaises(HTTPError) as error:
                urlopen(self.base + path)
            self.assertEqual(error.exception.code, 404)
            error.exception.close()
        with self.assertRaises(HTTPError) as error:
            urlopen(Request(self.url(), data=b'{}', method='POST'))
        self.assertEqual(error.exception.code, 501)
        error.exception.close()

    def test_key_survives_restart_and_rotation_revokes_old_url(self):
        old = self.url()
        self.assertEqual(Subscription(self.store).key, self.feed.key)
        self.feed.rotate()
        with self.assertRaises(HTTPError) as error:
            urlopen(old)
        self.assertEqual(error.exception.code, 404)
        error.exception.close()
        with urlopen(self.url()) as response:
            self.assertEqual(response.status, 200)
        self.assertEqual(Subscription(self.store).key, self.feed.key)


if __name__ == '__main__':
    unittest.main()
