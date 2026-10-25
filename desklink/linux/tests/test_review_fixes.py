import unittest
from unittest import mock

from gi.repository import GLib

from desklink import link, player, settings, ui, volume
from desklink.settings import Settings
from tests import fakes
from tests.test_link import track


class SeekTest(unittest.TestCase):
    def test_relative_seek_starts_from_where_it_plays_now(self):
        playing = track(elapsed=0.0, at=0.0, duration=600.0)
        method, args = player.seek_call(90.0, "", playing, now=60.0)
        self.assertEqual((method, args), ("Seek", (30_000_000,)))

    def test_absolute_seek_with_a_trackid(self):
        method, args = player.seek_call(90.0, "/org/mpris/1", track(), now=0.0)
        self.assertEqual((method, args), ("SetPosition", ("/org/mpris/1", 90_000_000)))


class LinkFixesTest(unittest.TestCase):
    def make(self, **client):
        self.timers = fakes.FakeTimers()
        self.clock = fakes.FakeClock(self.timers)
        self.client = fakes.FakeClient(self.clock, **client)
        self.link = link.Link(settings=Settings(), save_settings=lambda s: None, key=fakes.KEY_HEX,
                              client=self.client, volume=fakes.FakeVolume(), covers=fakes.FakeCovers(),
                              player_command=lambda *a: True, clock=self.clock, timers=self.timers,
                              machine="CloudPad", locked=lambda: False)
        self.link.start()

    def test_sharing_off_while_a_track_vanishes_leaves_no_ghost(self):
        self.make()
        self.link.take(track())
        self.link.take(None)
        self.link.set_sharing(False)
        self.assertIsNone(self.link.track)

    def test_an_empty_key_is_refused_not_stored(self):
        self.make()
        self.assertFalse(self.link.set_key(""))
        self.assertTrue(self.link.has_key())

    def test_a_reply_from_before_the_address_changed_is_ignored(self):
        self.make(defer=True)
        stale = self.client.pending.pop()
        self.link.set_panel("10.1.20.32")
        stale(None, b"")
        self.assertEqual(self.link.reach, link.Reach.UNKNOWN)


class ToggleGuardTest(unittest.TestCase):
    def test_a_click_that_took_the_focus_does_not_reopen_it(self):
        guard = ui.ToggleGuard(clock=lambda: self.now)
        self.now = 10.0
        guard.hidden_by_focus()
        self.now = 10.2
        self.assertFalse(guard.may_open())
        self.now = 11.0
        self.assertTrue(guard.may_open())


class KeyringTest(unittest.TestCase):
    def test_no_secret_service_reads_as_no_key(self):
        failing = mock.Mock(side_effect=GLib.Error("no secret service"))
        with mock.patch.object(settings.Secret, "password_lookup_sync", failing):
            self.assertEqual(settings.SecretKeyStore().read(), "")


class VolumeTimerTest(unittest.TestCase):
    def test_polls_on_the_guarded_timers(self):
        timers = mock.Mock()
        volume.Volume(lambda: None, run=lambda argv, done: done(False, ""), timers=timers).start()
        timers.every.assert_called_once()
