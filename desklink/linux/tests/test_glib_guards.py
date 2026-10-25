import time
import unittest

from gi.repository import GLib

from desklink import artwork, link
from desklink.server import PanelClient


def spin(until, seconds=3.0):
    context = GLib.MainContext.default()
    end = time.monotonic() + seconds
    while not until() and time.monotonic() < end:
        context.iteration(False)
        time.sleep(0.005)


class GuardTest(unittest.TestCase):
    def test_a_failing_heartbeat_keeps_beating(self):
        calls = []

        def beat():
            calls.append(1)
            raise RuntimeError("report failed")

        timer = link.GLibTimers().every(0.01, beat)
        spin(lambda: len(calls) >= 3)
        timer.cancel()
        self.assertGreaterEqual(len(calls), 3)

    def test_unparsable_panel_address_is_unreachable(self):
        got = []
        for url in ("http://smart flexispot/link", "http://fe80::1/link"):
            PanelClient().post(url, b"x", lambda status, body: got.append(status))
        spin(lambda: len(got) == 2)
        self.assertEqual(got, [None, None])

    def test_unparsable_cover_url_is_no_cover(self):
        got = []
        artwork.Covers().load("http://bad host/a.png", got.append)
        spin(lambda: got)
        self.assertEqual(got, [None])
