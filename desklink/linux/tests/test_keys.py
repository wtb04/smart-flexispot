import unittest

from desklink import keys


class SlideKeysTest(unittest.TestCase):
    def test_wtype_first(self):
        which = {"wtype": "/usr/bin/wtype", "ydotool": "/usr/bin/ydotool"}.get
        self.assertEqual(keys.argv_for(True, which), ["wtype", "-k", "Next"])
        self.assertEqual(keys.argv_for(False, which), ["wtype", "-k", "Prior"])

    def test_ydotool_without_wtype(self):
        which = {"ydotool": "/usr/bin/ydotool"}.get
        self.assertEqual(keys.argv_for(True, which), ["ydotool", "key", "109:1", "109:0"])
        self.assertEqual(keys.argv_for(False, which), ["ydotool", "key", "104:1", "104:0"])

    def test_neither_takes_no_slides(self):
        ran = []
        slide_keys = keys.SlideKeys(run=lambda argv, done: ran.append(argv), which=lambda name: None)
        self.assertFalse(slide_keys.available)
        self.assertFalse(slide_keys.press(True))
        self.assertEqual(ran, [])

    def test_press_runs_it(self):
        ran = []
        slide_keys = keys.SlideKeys(run=lambda argv, done: ran.append(argv), which={"wtype": "/usr/bin/wtype"}.get)
        self.assertTrue(slide_keys.press(False))
        self.assertEqual(ran, [["wtype", "-k", "Prior"]])


if __name__ == "__main__":
    unittest.main()
