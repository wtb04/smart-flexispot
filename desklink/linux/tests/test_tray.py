import unittest

from desklink import tray


class IconTest(unittest.TestCase):
    def test_icons(self):
        self.assertEqual(tray.icon_for("off", True), "desklink-off-symbolic")
        self.assertEqual(tray.icon_for("trouble", False), "desklink-trouble-symbolic")
        self.assertEqual(tray.icon_for("good", True), "desklink-playing-symbolic")
        self.assertEqual(tray.icon_for("checking", False), "desklink-display-symbolic")


class BarStatusTest(unittest.TestCase):
    def test_payload(self):
        self.assertEqual(tray.bar_status("good", True, "Connected to smart-flexispot"),
                         {"alt": "playing", "text": "", "tooltip": "Desk Link: Connected to smart-flexispot"})
        self.assertEqual(tray.bar_status("checking", False, "Looking for x")["alt"], "display")
        self.assertEqual(tray.bar_status("off", True, "Not sharing")["alt"], "off")
        self.assertEqual(tray.bar_status("trouble", True, "x not reachable")["alt"], "trouble")

    def test_written_atomically_as_one_line(self):
        import json
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "desklink" / "status.json"
            tray.write_bar_status(path, {"alt": "off"})
            tray.write_bar_status(path, {"alt": "display"})
            self.assertEqual(path.read_text().count("\n"), 1)
            self.assertEqual(json.loads(path.read_text()), {"alt": "display"})
            self.assertEqual(list(path.parent.iterdir()), [path])
