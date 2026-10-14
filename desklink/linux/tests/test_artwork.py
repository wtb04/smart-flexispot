import hashlib
import io
import tempfile
import unittest
from pathlib import Path

from gi.repository import GLib
from PIL import Image

from desklink import artwork


def png(width, height):
    out = io.BytesIO()
    Image.new("RGB", (width, height), (200, 120, 30)).save(out, "PNG")
    return out.getvalue()


def wait(loop_until):
    context = GLib.MainContext.default()
    for _ in range(500):
        if loop_until():
            return
        context.iteration(True)


class ArtworkTest(unittest.TestCase):
    def test_scales_to_480_on_multiples_of_8(self):
        original = png(1280, 720)
        art = artwork.make(original)
        image = Image.open(io.BytesIO(art.jpeg))
        self.assertEqual((image.format, image.size), ("JPEG", (480, 264)))
        self.assertFalse(image.info.get("progressive", False))
        self.assertTrue(art.wide)
        self.assertEqual(art.version, hashlib.sha256(original).digest()[:6].hex())

    def test_small_square_keeps_size_floored_to_8(self):
        art = artwork.make(png(301, 301))
        self.assertEqual(Image.open(io.BytesIO(art.jpeg)).size, (296, 296))
        self.assertFalse(art.wide)

    def test_tiny_is_at_least_8(self):
        self.assertEqual(Image.open(io.BytesIO(artwork.make(png(3, 3)).jpeg)).size, (8, 8))

    def test_not_an_image(self):
        self.assertIsNone(artwork.make(b"<html>nope</html>"))
        self.assertIsNone(artwork.make(b""))

    def test_covers_reads_files_and_refuses_the_rest(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "cover.png"
            original = png(10, 10)
            path.write_bytes(original)
            got = []
            covers = artwork.Covers()
            covers.load(path.as_uri(), got.append)
            covers.load(Path(folder, "missing.png").as_uri(), got.append)
            covers.load("data:image/png;base64,AAAA", got.append)
            wait(lambda: len(got) == 3)
        self.assertIn(original, got)
        self.assertEqual(got.count(None), 2)
