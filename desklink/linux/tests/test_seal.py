import unittest

from desklink import seal

KEY = bytes(range(32))


class SealTest(unittest.TestCase):
    def test_round_trip(self):
        body = seal.seal(KEY, "/link", b'{"type":"ping","at":1}')
        self.assertEqual(body[:3], b"DL1")
        plain, nonce = seal.open_(KEY, "/link", body)
        self.assertEqual(plain, b'{"type":"ping","at":1}')
        self.assertEqual(nonce, body[3:15])

    def test_bound_to_path(self):
        body = seal.seal(KEY, "/cover", b"x")
        with self.assertRaises(seal.Forged):
            seal.open_(KEY, "/link", body)

    def test_short_and_bad_magic_are_malformed(self):
        with self.assertRaises(seal.Malformed):
            seal.open_(KEY, "/link", b"DL1" + bytes(27))
        with self.assertRaises(seal.Malformed):
            seal.open_(KEY, "/link", b"XL1" + bytes(40))

    def test_parse_key(self):
        self.assertEqual(seal.parse_key("00" * 32), bytes(32))
        self.assertEqual(seal.parse_key("AB" * 32), bytes([0xAB]) * 32)
        self.assertIsNone(seal.parse_key("00" * 31))
        self.assertIsNone(seal.parse_key("zz" * 32))
        self.assertIsNone(seal.parse_key("0 " * 32))

    def test_fresh_window_and_replay(self):
        fresh = seal.Fresh()
        self.assertTrue(fresh.take(b"a" * 12, 1000, 1000))
        self.assertFalse(fresh.take(b"a" * 12, 1000, 1000))
        self.assertTrue(fresh.take(b"b" * 12, 1000 - 60000, 1000))
        self.assertFalse(fresh.take(b"c" * 12, 1000 - 60001, 1000))

    def test_fresh_forgets_after_64(self):
        fresh = seal.Fresh()
        for i in range(65):
            self.assertTrue(fresh.take(i.to_bytes(12, "big"), 5, 5))
        self.assertTrue(fresh.take((0).to_bytes(12, "big"), 5, 5))


class EnvelopeTest(unittest.TestCase):
    def setUp(self):
        self.now = 1_791_223_792_000
        self.envelope = seal.Envelope(KEY, clock_ms=lambda: self.now)

    def test_message_round_trip_adds_at(self):
        body = self.envelope.sealed({"type": "ping"}, "/link")
        self.assertEqual(self.envelope.opened(body, "/link"), {"type": "ping", "at": self.now})

    def test_refuses_replay_stale_and_garbage(self):
        body = self.envelope.sealed({"type": "ping"}, "/link")
        self.assertIsNotNone(self.envelope.opened(body, "/link"))
        self.assertIsNone(self.envelope.opened(body, "/link"))
        stale = seal.Envelope(KEY, clock_ms=lambda: self.now - 61_000).sealed({"type": "ping"}, "/link")
        self.assertIsNone(self.envelope.opened(stale, "/link"))
        self.assertIsNone(self.envelope.opened(b"hello", "/link"))
        not_json = seal.seal(KEY, "/link", b"\xff\xfe")
        self.assertIsNone(self.envelope.opened(not_json, "/link"))

    def test_refuses_without_type_or_at(self):
        for plain in (b'{"at":1791223792000}', b'{"type":"ping"}', b'{"type":"ping","at":0}', b"[1]"):
            self.assertIsNone(self.envelope.opened(seal.seal(KEY, "/link", plain), "/link"), plain)
