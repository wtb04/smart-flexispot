import json
import tempfile
import unittest
from pathlib import Path

from desklink import settings


class SettingsTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = Path(self.dir.name) / "desklink" / "settings.json"

    def tearDown(self):
        self.dir.cleanup()

    def test_defaults_when_missing(self):
        self.assertEqual(settings.load(self.path), settings.Settings())

    def test_round_trip(self):
        saved = settings.Settings(panel="127.0.0.1:8080", sharing=False, claude=False)
        settings.save(saved, self.path)
        self.assertEqual(settings.load(self.path), saved)

    def test_bad_file_and_wrong_types_fall_back(self):
        self.path.parent.mkdir(parents=True)
        self.path.write_text("{not json")
        self.assertEqual(settings.load(self.path), settings.Settings())
        self.path.write_text(json.dumps({"panel": 5, "sharing": "yes", "claude": False, "extra": 1}))
        self.assertEqual(settings.load(self.path), settings.Settings(claude=False))
