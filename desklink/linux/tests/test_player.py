import unittest

from desklink import player


def track(title="Song", playing=True, identity="Spotify", **more):
    values = dict(identity=identity, title=title, artist="Artist", playing=playing, duration=200.0,
                  elapsed=10.0, at=100.0, rate=1.0, can_seek=True, can_next=True, can_previous=True,
                  art_url="")
    values.update(more)
    return player.Track(**values)


class TrackTest(unittest.TestCase):
    def test_position_extrapolates_and_caps(self):
        self.assertEqual(track().position(105.0), 15.0)
        self.assertEqual(track(playing=False).position(105.0), 10.0)
        self.assertEqual(track().position(1000.0), 200.0)
        self.assertEqual(track(duration=0.0).position(1000.0), 910.0)

    def test_browser(self):
        self.assertTrue(track(identity="Google Chrome").browser)
        self.assertTrue(track(identity="Microsoft Edge").browser)
        self.assertFalse(track(identity="Spotify").browser)


class ChooseTest(unittest.TestCase):
    def test_playing_wins_and_latest_start_among_them(self):
        seen = {"a": player.Seen(track(playing=False), 50.0, 0),
                "b": player.Seen(track(), 10.0, 1),
                "c": player.Seen(track(), 20.0, 2)}
        self.assertEqual(player.choose(seen), "c")

    def test_none_playing_takes_most_recent(self):
        seen = {"a": player.Seen(track(playing=False), 0.0, 0),
                "b": player.Seen(track(playing=False), 30.0, 1)}
        self.assertEqual(player.choose(seen), "b")

    def test_untitled_does_not_count(self):
        self.assertIsNone(player.choose({"a": player.Seen(track(title=""), 99.0, 0)}))
        self.assertIsNone(player.choose({}))


class TrackFromTest(unittest.TestCase):
    def test_reads_mpris_properties(self):
        props = {"PlaybackStatus": "Playing", "Rate": 1.0, "CanSeek": True, "CanGoNext": False,
                 "CanGoPrevious": True,
                 "Metadata": {"xesam:title": "Song", "xesam:artist": ["A", "B"], "mpris:length": 200_000_000,
                              "mpris:artUrl": "file:///tmp/a.png"}}
        got = player.track_from("Spotify", props, 5_000_000, 42.0)
        self.assertEqual((got.title, got.artist, got.duration, got.elapsed, got.at, got.can_next),
                         ("Song", "A, B", 200.0, 5.0, 42.0, False))

    def test_stopped_or_untitled_is_none(self):
        self.assertIsNone(player.track_from("x", {"PlaybackStatus": "Stopped",
                                                  "Metadata": {"xesam:title": "Song"}}, 0, 0.0))
        self.assertIsNone(player.track_from("x", {"PlaybackStatus": "Playing", "Metadata": {}}, 0, 0.0))


class CommandTest(unittest.TestCase):
    def test_command_without_a_player_is_a_no_op(self):
        mpris = player.Mpris(lambda _track: None)
        self.assertFalse(mpris.command("pause"))
        self.assertFalse(mpris.command("seek", 30.0))
