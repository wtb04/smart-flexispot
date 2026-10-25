import io
import json
import unittest
from pathlib import Path

from PIL import Image, ImageOps

from desklink import youtube

PAGE = (Path(__file__).parent / "fixtures" / "youtube-search.html").read_text(encoding="utf-8")
TITLE, CHANNEL = "We Raced The Entire Length Of Japan - Ep 6", "Jet Lag: The Game"


def jpeg(image):
    out = io.BytesIO()
    image.save(out, "JPEG", quality=90)
    return out.getvalue()


def frame(width=1280, height=720):
    image = Image.new("RGB", (width, height))
    for x in range(width):
        for y in range(0, height, 8):
            image.putpixel((x, y), (x * 255 // width, y * 255 // height, 90))
    return image.resize((width, height))


class PageTest(unittest.TestCase):
    def test_finds_the_exact_video_not_the_first(self):
        video = youtube.find_video(youtube.initial_data(PAGE), TITLE, CHANNEL)
        self.assertEqual((video.id, video.short), ("xBJ3EvFzHRk", False))
        self.assertTrue(video.avatar.endswith("=s480-c-k-c0x00ffffff-no-rj"), video.avatar)

    def test_match_ignores_case_and_needs_the_channel(self):
        data = youtube.initial_data(PAGE)
        self.assertEqual(youtube.find_video(data, TITLE.upper(), CHANNEL.lower()).id, "xBJ3EvFzHRk")
        self.assertIsNone(youtube.find_video(data, TITLE, "Someone Else"))
        self.assertEqual(youtube.find_video(data, TITLE, "").id, "xBJ3EvFzHRk")
        self.assertIsNone(youtube.find_video(data, "Not On The Page", CHANNEL))

    def test_not_a_search_page(self):
        self.assertIsNone(youtube.initial_data("<html>consent</html>"))
        self.assertIsNone(youtube.initial_data("var ytInitialData = {broken;</script>"))

    def test_short_found_by_its_title(self):
        data = {"a": [{"shortsLockupViewModel": {"accessibilityText": "Tokyo In 60 Seconds, 2 million views",
                                                  "entityId": "shorts-shelf-item-abc123"}}]}
        self.assertEqual(youtube.find_short(data, "tokyo in 60 seconds"), "abc123")
        self.assertIsNone(youtube.find_short(data, "tokyo"))

    def test_og_image_at_the_panels_size(self):
        html = '<meta property="og:image" content="https://yt3.googleusercontent.com/x=s900-c-k">'
        self.assertEqual(youtube.og_image(html), "https://yt3.googleusercontent.com/x=s480-c-k")
        self.assertIsNone(youtube.og_image("<html></html>"))


class DifferenceTest(unittest.TestCase):
    def test_same_frame_smaller_is_close_and_another_is_far(self):
        big = frame()
        small = jpeg(big.resize((150, 83)))
        self.assertLess(youtube.difference(small, jpeg(big)), youtube.SAME_ENOUGH)
        self.assertGreater(youtube.difference(small, jpeg(ImageOps.invert(big))), youtube.SAME_ENOUGH)
        self.assertIsNone(youtube.difference(b"nope", jpeg(big)))


class FindTest(unittest.TestCase):
    def setUp(self):
        self.pages = {}
        self.asked = []

    def fetch(self, url, done):
        self.asked.append(url)
        done(self.pages.get(url.split("?")[0] if "results" in url or "oembed" in url else url))

    def find(self, small, title=TITLE, channel=CHANNEL):
        got = []
        youtube.YouTube(fetch=self.fetch).find(title, channel, small, got.append)
        return got[0]

    def test_full_size_thumbnail_and_channel_picture(self):
        thumb = jpeg(frame())
        self.pages = {"https://www.youtube.com/results": PAGE.encode(),
                      "https://i.ytimg.com/vi/xBJ3EvFzHRk/maxresdefault.jpg": thumb}
        avatar_url = youtube.find_video(youtube.initial_data(PAGE), TITLE, CHANNEL).avatar
        self.pages[avatar_url] = b"AVATAR"
        found = self.find(jpeg(frame().resize((150, 83))))
        self.assertEqual((found.thumbnail, found.channel), (thumb, b"AVATAR"))
        self.assertIn("search_query=We+Raced", self.asked[0])

    def test_falls_back_to_mq_and_works_without_a_small_cover(self):
        thumb = jpeg(frame(320, 180))
        self.pages = {"https://www.youtube.com/results": PAGE.encode(),
                      "https://i.ytimg.com/vi/xBJ3EvFzHRk/mqdefault.jpg": thumb}
        found = self.find(None)
        self.assertEqual((found.thumbnail, found.channel), (thumb, None))

    def test_refuses_a_thumbnail_unlike_the_small_cover(self):
        self.pages = {"https://www.youtube.com/results": PAGE.encode(),
                      "https://i.ytimg.com/vi/xBJ3EvFzHRk/maxresdefault.jpg": jpeg(ImageOps.invert(frame()))}
        self.assertIsNone(self.find(jpeg(frame().resize((150, 83)))))

    def test_nothing_found(self):
        self.assertIsNone(self.find(None))
        self.pages = {"https://www.youtube.com/results": PAGE.encode()}
        self.assertIsNone(self.find(None, title="Not On The Page"))

    def test_short_through_oembed_and_its_channel_page(self):
        data = {"x": [{"shortsLockupViewModel": {"accessibilityText": "Tokyo In 60 Seconds, 2M views",
                                                  "entityId": "shorts-shelf-item-abc123"}}]}
        self.pages = {
            "https://www.youtube.com/results": f"var ytInitialData = {json.dumps(data)};</script>".encode(),
            "https://www.youtube.com/oembed": json.dumps({"author_name": "Jet Lag: The Game",
                                                         "author_url": "https://www.youtube.com/@jetlag"}).encode(),
            "https://www.youtube.com/@jetlag": b'<meta property="og:image" content="https://yt3/x=s900-c">',
            "https://i.ytimg.com/vi/abc123/maxresdefault.jpg": jpeg(ImageOps.invert(frame())),
            "https://yt3/x=s480-c": b"AVATAR",
        }
        found = self.find(jpeg(frame().resize((150, 83))), title="Tokyo In 60 Seconds")
        self.assertEqual(found.channel, b"AVATAR")  # a Short skips the likeness check
        self.pages["https://www.youtube.com/oembed"] = json.dumps({"author_name": "Other"}).encode()
        self.assertIsNone(self.find(None, title="Tokyo In 60 Seconds"))
