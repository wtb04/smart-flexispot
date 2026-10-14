import socket
import subprocess
import unittest

from gi.repository import GLib

from desklink.link import Response
from desklink.server import PanelClient, Server


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def curl(args):
    process = subprocess.Popen(["curl", "-s", "-o", "/dev/null", "-w", "%{http_code}", "-m", "3", *args],
                               stdout=subprocess.PIPE, text=True)
    context = GLib.MainContext.default()
    while process.poll() is None:
        context.iteration(False)
    return process.stdout.read()


class ServerTest(unittest.TestCase):
    def setUp(self):
        self.seen = []
        self.port = free_port()
        self.server = Server(self.handle, self.port)
        self.assertIsNone(self.server.start())

    def tearDown(self):
        self.server.stop()

    def handle(self, method, path, body, local):
        self.seen.append((method, path, body, local))
        if path == "/boom":
            raise RuntimeError("handler bug")
        return Response(204) if path == "/claude" else Response(200, "application/octet-stream", b"ok")

    def test_passes_requests_through(self):
        url = f"http://127.0.0.1:{self.port}"
        self.assertEqual(curl(["-X", "POST", "--data-binary", "abc", url + "/link"]), "200")
        self.assertEqual(self.seen[-1], ("POST", "/link", b"abc", True))
        self.assertEqual(curl(["-X", "POST", "-d", "{}", url + "/claude"]), "204")

    def test_garbage_never_breaks_it(self):
        url = f"http://127.0.0.1:{self.port}"
        self.assertEqual(curl([url + "/"]), "200")
        self.assertEqual(curl(["-X", "POST", url + "/boom"]), "500")
        self.assertEqual(curl(["-X", "POST", url + "/link"]), "200")
        with socket.create_connection(("127.0.0.1", self.port)) as raw:
            raw.sendall(b"NONSENSE\r\n\r\n")
        self.assertEqual(curl([url + "/link"]), "200")

    def test_port_in_use_is_reported(self):
        self.assertEqual(Server(self.handle, self.port).start(), f"port {self.port} in use")

    def test_oversized_is_refused(self):
        url = f"http://127.0.0.1:{self.port}/link"
        with open("/dev/zero", "rb") as zero:
            data = zero.read(64 * 1024 + 1)
        process = subprocess.Popen(["curl", "-s", "-o", "/dev/null", "-w", "%{http_code}", "-m", "3", "-X", "POST",
                                    "--data-binary", "@-", url], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        process.stdin.write(data)
        process.stdin.close()
        context = GLib.MainContext.default()
        while process.poll() is None:
            context.iteration(False)
        self.assertEqual(process.stdout.read(), b"413")


class ClientTest(unittest.TestCase):
    def test_unreachable_is_none(self):
        got = []
        PanelClient(timeout_s=1).post("http://nonexistent.invalid/link", b"x", lambda s, b: got.append(s))
        context = GLib.MainContext.default()
        for _ in range(2000):
            if got:
                break
            context.iteration(True)
        self.assertEqual(got, [None])
