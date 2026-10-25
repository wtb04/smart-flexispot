import json
import unittest

from desklink import artwork, link, seal
from desklink.player import Track
from desklink.settings import Settings
from tests import fakes
from tests.test_conformance import TOOL, run_tool


def track(title="Song", playing=True, **more):
    values = dict(identity="Spotify", title=title, artist="Artist", playing=playing, duration=200.0,
                  elapsed=10.0, at=1000.0, rate=1.0, can_seek=True, can_next=True, can_previous=True,
                  art_url="")
    values.update(more)
    return Track(**values)


class BuildStateTest(unittest.TestCase):
    def test_nothing_is_five_fields(self):
        self.assertEqual(link.build_state("CloudPad", 47801, None, None, fakes.FakeVolume(), 0.0),
                         {"type": "state", "machine": "CloudPad", "port": 47801, "title": ""})

    def test_full_state(self):
        art = artwork.Artwork(b"jpeg", "3f2a1b0c9d8e", True)
        state = link.build_state("CloudPad", 47801, track(), art, fakes.FakeVolume(40, True), 1005.0)
        self.assertEqual(state["takes"], ["pause", "seek", "next", "previous", "volume"])
        self.assertEqual((state["position"], state["duration"], state["art"], state["video"]), (15.0, 200.0,
                                                                                               "3f2a1b0c9d8e", True))
        self.assertEqual((state["volume"], state["muted"], state["square"], state["app"]), (40, True, "", "Spotify"))

    def test_takes_follow_the_player_and_output(self):
        state = link.build_state("m", 1, track(duration=0.0, can_next=False, can_previous=False), None,
                                 fakes.FakeVolume(None), 1000.0)
        self.assertEqual(state["takes"], ["pause"])
        self.assertNotIn("volume", state)

    def test_browser_before_a_cover_is_video(self):
        self.assertTrue(link.build_state("m", 1, track(identity="Google Chrome"), None, fakes.FakeVolume(),
                                         1000.0)["video"])
        self.assertFalse(link.build_state("m", 1, track(), None, fakes.FakeVolume(), 1000.0)["video"])

    @unittest.skipUnless(TOOL.exists(), "conformance tool not built")
    def test_panel_reads_it_as_meant(self):
        art = artwork.Artwork(b"jpeg", "3f2a1b0c9d8e", False)
        state = link.build_state("CloudPad", 47801, track(), art, fakes.FakeVolume(40, False), 1005.0)
        read = json.loads(run_tool(["read-state"], json.dumps({**state, "at": 1}).encode()).stdout)
        self.assertEqual((read["ok"], read["active"], read["playing"], read["title"], read["position_s"],
                          read["duration_s"], read["volume"], read["takes_next"], read["takes_volume"]),
                         (True, True, True, "Song", 15, 200, 40, True, True))
        nothing = json.loads(run_tool(["read-state"], json.dumps(
            {**link.build_state("CloudPad", 47801, None, None, fakes.FakeVolume(), 0.0), "at": 1}).encode()).stdout)
        self.assertEqual((nothing["ok"], nothing["active"]), (True, False))


class LinkTest(unittest.TestCase):
    def make(self, settings=None, key=fakes.KEY_HEX, **client):
        self.timers = fakes.FakeTimers()
        self.clock = fakes.FakeClock(self.timers)
        self.client = fakes.FakeClient(self.clock, **client)
        self.volume = fakes.FakeVolume()
        self.covers = fakes.FakeCovers()
        self.commands = []
        self.locked = False
        self.saved = []
        self.link = link.Link(settings=settings or Settings(), save_settings=self.saved.append, key=key,
                              client=self.client, volume=self.volume, covers=self.covers,
                              player_command=lambda name, position=None: self.commands.append((name, position)) or True,
                              clock=self.clock, timers=self.timers, machine="CloudPad", locked=lambda: self.locked)
        self.panel = seal.Envelope(fakes.KEY, clock_ms=self.clock.wall_ms)
        self.link.start()

    def test_reports_at_start_and_every_ten_seconds(self):
        self.make()
        self.timers.advance(25)
        self.assertEqual(len(self.client.states()), 3)
        self.assertEqual(self.link.status_text(), "Connected to smart-flexispot")

    def test_changes_coalesce_but_pause_goes_at_once(self):
        self.make()
        self.link.take(track())
        self.link.take(track(elapsed=11.0))
        self.assertEqual(len(self.client.states()), 1)
        self.timers.advance(0.3)
        self.assertEqual(len(self.client.states()), 2)
        self.link.take(track(playing=False))
        self.assertEqual(self.client.states()[-1]["playing"], False)

    def test_vanish_waits_three_seconds(self):
        self.make()
        self.link.take(track())
        self.timers.advance(0.3)
        self.link.take(None)
        self.timers.advance(2.9)
        self.assertEqual(self.client.states()[-1]["title"], "Song")
        self.link.take(track())
        self.timers.advance(0.4)
        self.assertEqual(self.client.states()[-1]["title"], "Song")
        self.link.take(None)
        self.timers.advance(3.4)  # the 3 s wait, then the 0.3 s coalescing
        self.assertEqual(self.client.states()[-1]["title"], "")

    def test_cover_loaded_for_the_track_and_dropped_for_another(self):
        self.make()
        self.link.take(track(art_url="file:///a.png"))
        url, done = self.covers.asked[-1]
        self.assertEqual(url, "file:///a.png")
        import io
        from PIL import Image
        out = io.BytesIO()
        Image.new("RGB", (64, 64)).save(out, "PNG")
        done(out.getvalue())
        self.timers.advance(0.3)
        self.assertEqual(len(self.client.states()[-1]["art"]), 12)
        self.link.take(track(title="Other", art_url=""))
        self.timers.advance(0.3)
        self.assertEqual(self.client.states()[-1]["art"], "")

    def test_bad_cover_leaves_art_empty(self):
        self.make()
        self.link.take(track(art_url="file:///bad"))
        self.covers.asked[-1][1](b"not an image")
        self.timers.advance(0.3)
        self.assertEqual(self.client.states()[-1]["art"], "")

    def test_away_after_idle_while_locked_then_silent(self):
        self.make()
        self.link.take(track(playing=False))
        self.locked = True
        self.timers.advance(135)  # the beat at +130 s is the first past 120 s
        count = len(self.client.posts)
        self.assertEqual(self.client.states()[-1]["title"], "")
        self.timers.advance(60)
        self.assertEqual(len(self.client.posts), count)
        self.locked = False
        self.timers.advance(10)
        self.assertEqual(self.client.states()[-1]["title"], "Song")

    def test_sleep_goes_away_at_once(self):
        self.make()
        self.link.take(track())
        self.timers.advance(0.3)
        self.link.set_sleeping(True)
        self.assertEqual(self.client.states()[-1]["title"], "")

    def test_statuses(self):
        self.make(key="")
        self.assertEqual(self.link.status_text(), "Needs the panel's key")
        self.assertEqual(self.client.posts, [])
        self.make(status=None)
        self.assertEqual(self.link.status_text(), "smart-flexispot not reachable")
        self.assertEqual(self.link.health(), "trouble")
        self.timers.advance(10)
        self.assertEqual(len(self.client.posts), 2)
        self.make(status=403)
        self.assertEqual(self.link.status_text(), "Wrong key, or not the panel")
        self.make(reaches=False)
        self.assertEqual(self.link.status_text(), "smart-flexispot cannot reach this laptop")
        self.make(reaches=None)
        self.assertEqual(self.link.status_text(), "Connected, checking back")
        self.make(settings=Settings(sharing=False))
        self.assertEqual(self.link.status_text(), "Not sharing")
        self.assertEqual(self.link.health(), "off")

    def test_turning_sharing_off_lets_go_at_once(self):
        self.make()
        self.link.take(track())
        self.timers.advance(0.3)
        self.link.set_sharing(False)
        self.assertEqual(self.client.states()[-1]["title"], "")
        self.assertEqual(self.saved[-1].sharing, False)
        count = len(self.client.posts)
        self.timers.advance(30)
        self.assertEqual(len(self.client.posts), count)

    def test_panel_commands(self):
        self.make()
        self.link.take(track())
        for message, expected in (({"command": "pause"}, ("pause", None)),
                                  ({"command": "seek", "position": 30}, ("seek", 30.0))):
            reply = self.link.handle("POST", "/link", self.panel.sealed({"type": "command", **message}, "/link"), False)
            self.assertEqual(reply.status, 200)
            self.assertEqual(self.panel.opened(reply.body, "/link reply")["type"], "ok")
            self.assertEqual(self.commands[-1], expected)
        reply = self.link.handle("POST", "/link", self.panel.sealed({"type": "command", "command": "volume",
                                                                     "level": 55}, "/link"), False)
        self.assertEqual((reply.status, self.volume.calls[-1]), (200, ("volume", 55)))
        for bad in ({"command": "seek"}, {"command": "mute", "muted": 1}, {"command": "fly"},
                    {"command": "volume", "level": True}):
            reply = self.link.handle("POST", "/link", self.panel.sealed({"type": "command", **bad}, "/link"), False)
            self.assertEqual(reply.status, 400, bad)

    def test_ping_and_cover(self):
        self.make()
        reply = self.link.handle("POST", "/link", self.panel.sealed({"type": "ping"}, "/link"), False)
        self.assertEqual(self.panel.opened(reply.body, "/link reply")["type"], "pong")
        self.link.artwork = artwork.Artwork(b"JPEGDATA", "abcdefabcdef", False)
        reply = self.link.handle("POST", "/cover", self.panel.sealed({"type": "cover", "art": "abcdefabcdef"},
                                                                     "/cover"), False)
        self.assertEqual(seal.open_(fakes.KEY, "/cover reply", reply.body)[0], b"JPEGDATA")
        reply = self.link.handle("POST", "/cover", self.panel.sealed({"type": "cover", "art": "000000000000"},
                                                                     "/cover"), False)
        self.assertEqual(reply.status, 404)

    def test_refuses_what_is_not_the_panels(self):
        self.make()
        for method, path, body in (("POST", "/link", b"garbage"), ("GET", "/link", b""), ("POST", "/other", b""),
                                   ("POST", "/link", b"")):
            self.assertEqual(self.link.handle(method, path, body, False).status, 403, (method, path))
        replay = self.panel.sealed({"type": "ping"}, "/link")
        self.link.handle("POST", "/link", replay, False)
        self.assertEqual(self.link.handle("POST", "/link", replay, False).status, 403)
        unknown = self.panel.sealed({"type": "dance"}, "/link")
        self.assertEqual(self.link.handle("POST", "/link", unknown, False).status, 400)

    def test_claude_relay_local_only_and_switchable(self):
        self.make()
        event = json.dumps({"machine": "CloudPad", "event": "Stop"}).encode()
        self.assertEqual(self.link.handle("POST", "/claude", event, False).status, 403)
        self.assertEqual(self.link.handle("POST", "/claude", event, True).status, 204)
        self.assertEqual(self.client.posts[-1][1]["event"], {"machine": "CloudPad", "event": "Stop"})
        self.link.set_claude(False)
        count = len(self.client.posts)
        self.assertEqual(self.link.handle("POST", "/claude", event, True).status, 204)
        self.assertEqual(self.link.handle("POST", "/claude", b"not json", True).status, 204)
        self.assertEqual(len(self.client.posts), count)

    def test_set_key_validates(self):
        self.make(key="")
        self.assertFalse(self.link.set_key("nope"))
        self.assertTrue(self.link.set_key(fakes.KEY_HEX))
        self.assertTrue(self.link.has_key())


def png(width, height, colour=(200, 120, 30)):
    import io
    from PIL import Image
    out = io.BytesIO()
    Image.new("RGB", (width, height), colour).save(out, "PNG")
    return out.getvalue()


class YouTubeCoverTest(unittest.TestCase):
    def setUp(self):
        from desklink.youtube import Found
        self.Found = Found
        self.timers = fakes.FakeTimers()
        self.clock = fakes.FakeClock(self.timers)
        self.client = fakes.FakeClient(self.clock)
        self.covers = fakes.FakeCovers()
        self.youtube = fakes.FakeYouTube()
        self.link = link.Link(settings=Settings(), save_settings=lambda s: None, key=fakes.KEY_HEX,
                              client=self.client, volume=fakes.FakeVolume(), covers=self.covers,
                              player_command=lambda *a: True, clock=self.clock, timers=self.timers,
                              machine="CloudPad", locked=lambda: False, youtube=self.youtube)
        self.panel = seal.Envelope(fakes.KEY, clock_ms=self.clock.wall_ms)
        self.link.start()

    def chrome(self, title="We Raced The Entire Length Of Japan - Ep 6", art_url="file:///tmp/.chrome", **more):
        return track(identity="Google Chrome", title=title, artist="Jet Lag: The Game", art_url=art_url,
                     can_next=False, can_previous=False, **more)

    def test_browser_cover_is_replaced_by_the_videos_own(self):
        self.link.take(self.chrome())
        small = png(150, 83)
        self.covers.asked[-1][1](small)
        title, channel, given, done = self.youtube.asked[-1]
        self.assertEqual((title, channel, given), ("We Raced The Entire Length Of Japan - Ep 6", "Jet Lag: The Game",
                                                   small))
        self.timers.advance(0.3)
        self.assertEqual(len(self.client.states()[-1]["art"]), 12)
        thumbnail, channel_picture = png(1280, 720), png(480, 480, (10, 10, 10))
        done(self.Found(thumbnail, channel_picture))
        self.timers.advance(0.3)
        state = self.client.states()[-1]
        self.assertEqual(state["art"], artwork.make(thumbnail).version)
        self.assertEqual(state["square"], artwork.make(channel_picture).version)
        self.assertTrue(state["video"])
        reply = self.link.handle("POST", "/cover", self.panel.sealed({"type": "cover", "art": state["square"]},
                                                                     "/cover"), False)
        self.assertEqual(reply.status, 200)

    def test_no_small_cover_searches_at_once(self):
        self.link.take(self.chrome(art_url=""))
        self.assertIsNone(self.youtube.asked[-1][2])

    def test_one_search_per_video_and_none_for_other_players(self):
        self.link.take(self.chrome(art_url=""))
        self.link.take(self.chrome(art_url="", playing=False))
        self.assertEqual(len(self.youtube.asked), 1)
        self.link.take(track())
        self.assertEqual(len(self.youtube.asked), 1)

    def test_a_late_answer_for_another_video_is_ignored(self):
        self.link.take(self.chrome(art_url=""))
        done = self.youtube.asked[-1][3]
        self.link.take(self.chrome(title="Another Episode", art_url=""))
        done(self.Found(png(1280, 720), None))
        self.timers.advance(0.3)
        self.assertEqual(self.client.states()[-1]["art"], "")

    def test_not_found_keeps_the_small_cover(self):
        self.link.take(self.chrome())
        self.covers.asked[-1][1](png(150, 83))
        self.youtube.asked[-1][3](None)
        self.timers.advance(0.3)
        state = self.client.states()[-1]
        self.assertEqual((len(state["art"]), state["square"]), (12, ""))
