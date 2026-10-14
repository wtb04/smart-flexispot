import unittest
from types import SimpleNamespace

from desklink import ui


class StatusLineTest(unittest.TestCase):
    def test_server_error_wins(self):
        link = SimpleNamespace(status_text=lambda: "Connected to smart-flexispot")
        self.assertEqual(ui.status_line(link, "port 47801 in use"), "port 47801 in use")
        self.assertEqual(ui.status_line(link, None), "Connected to smart-flexispot")

    def test_subtitle(self):
        self.assertEqual(ui.subtitle(SimpleNamespace(artist="Ubiquiti", identity="Google Chrome")),
                         "Ubiquiti, Google Chrome")
        self.assertEqual(ui.subtitle(SimpleNamespace(artist="", identity="Spotify")), "Spotify")
