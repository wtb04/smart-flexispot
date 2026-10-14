import json
import os
import subprocess
import unittest
from pathlib import Path

from desklink import seal

CONFORMANCE = Path(__file__).resolve().parents[2] / "conformance"
TOOL = Path(os.environ.get("DESKLINK_CONFORMANCE", CONFORMANCE / "build" / "conformance"))
FIXTURES = json.loads((CONFORMANCE / "fixtures.json").read_text())
KEY = bytes.fromhex(FIXTURES["key"])


def run_tool(args, stdin: bytes) -> subprocess.CompletedProcess:
    return subprocess.run([str(TOOL), *args], input=stdin, capture_output=True,
                          env={**os.environ, "DESKLINK_KEY": FIXTURES["key"]}, check=False)


class FixturesTest(unittest.TestCase):
    def test_every_sealed_case(self):
        for case in FIXTURES["sealed"]:
            with self.subTest(case["name"]):
                body = bytes.fromhex(case["body"])
                if case["result"] == "ok":
                    plain, _ = seal.open_(KEY, case["opens_on"], body)
                    self.assertEqual(plain.decode(), case["plain"])
                else:
                    error = seal.Malformed if case["result"] == "malformed" else seal.Forged
                    with self.assertRaises(error):
                        seal.open_(KEY, case["opens_on"], body)

    def test_freshness_sequence(self):
        fresh = seal.Fresh()
        for step in FIXTURES["fresh"]:
            self.assertEqual(fresh.take(bytes.fromhex(step["nonce"]), int(step["at"]), FIXTURES["now_ms"]),
                             step["taken"], step)


@unittest.skipUnless(TOOL.exists(), f"build {TOOL} first, see desklink/conformance/README.md")
class ToolTest(unittest.TestCase):
    def test_panel_opens_what_python_seals(self):
        for path in ("/link", "/link reply", "/cover reply"):
            plain = b'{"type":"state","at":1791223792000,"port":47801,"title":""}'
            got = run_tool(["open", path], seal.seal(KEY, path, plain))
            self.assertEqual((got.returncode, got.stdout), (0, plain), path)

    def test_python_opens_what_panel_seals(self):
        got = run_tool(["seal", "/link reply"], b'{"type":"pong","at":5}')
        self.assertEqual(seal.open_(KEY, "/link reply", got.stdout)[0], b'{"type":"pong","at":5}')

    def test_panel_refuses_python_with_the_wrong_path(self):
        self.assertEqual(run_tool(["open", "/link"], seal.seal(KEY, "/cover", b"{}")).returncode, 3)
