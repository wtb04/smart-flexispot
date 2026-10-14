import unittest

from desklink import volume


class ParseTest(unittest.TestCase):
    def test_levels(self):
        self.assertEqual(volume.parse("Volume: 0.40\n"), (40, False))
        self.assertEqual(volume.parse("Volume: 0.40 [MUTED]\n"), (40, True))
        self.assertEqual(volume.parse("Volume: 1.50\n"), (100, False))
        self.assertEqual(volume.parse(""), (None, False))


class VolumeTest(unittest.TestCase):
    def setUp(self):
        self.calls = []
        self.answer = (True, "Volume: 0.30\n")
        self.changes = 0
        self.volume = volume.Volume(self.changed, run=self.fake_run)

    def changed(self):
        self.changes += 1

    def fake_run(self, argv, done):
        self.calls.append(argv)
        done(*self.answer)

    def test_refresh_reports_only_changes(self):
        self.volume.refresh()
        self.volume.refresh()
        self.assertEqual((self.volume.percent, self.changes), (30, 1))

    def test_missing_wpctl_means_no_volume(self):
        self.answer = (False, "")
        self.volume.refresh()
        self.assertIsNone(self.volume.percent)

    def test_set_and_step_clamp(self):
        self.volume.refresh()
        self.volume.set_percent(140)
        self.assertIn(["wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "1.00"], self.calls)
        self.assertTrue(self.volume.step(-5))
        self.assertIn(["wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "0.25"], self.calls)
        self.volume.set_muted(True)
        self.assertIn(["wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "1"], self.calls)

    def test_step_without_volume(self):
        self.answer = (False, "")
        self.volume.refresh()
        self.assertFalse(self.volume.step(5))
