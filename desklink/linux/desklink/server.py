"""HTTP both ways: the server on port 47801 the panel and claude-hook reach, and
the client that posts to the panel."""

import logging
from typing import Callable

import gi

gi.require_version("Soup", "3.0")
from gi.repository import GLib, Soup  # noqa: E402

from .link import Response  # noqa: E402

log = logging.getLogger(__name__)

LARGEST = 64 * 1024
LOCAL = {"127.0.0.1", "::1", "::ffff:127.0.0.1"}


class Server:
    def __init__(self, handle: Callable[[str, str, bytes, bool], Response], port: int = 47801):
        self._handle = handle
        self._port = port
        self._server: Soup.Server | None = None

    def start(self) -> str | None:
        server = Soup.Server()
        server.add_handler(None, self._serve)
        try:
            server.listen_all(self._port, 0)
        except GLib.Error as error:
            log.warning("cannot listen on %d: %s", self._port, error.message)
            return f"port {self._port} in use"
        self._server = server
        return None

    def stop(self) -> None:
        if self._server:
            self._server.disconnect()
            self._server = None

    def _serve(self, _server, message: Soup.ServerMessage, path: str, _query) -> None:
        message.get_response_headers().append("Connection", "close")
        body = message.get_request_body().flatten().get_data() or b""
        if len(body) > LARGEST:
            message.set_status(413, None)
            return
        try:
            response = self._handle(message.get_method(), path, body, message.get_remote_host() in LOCAL)
        except Exception:
            log.exception("handling %s", path)
            response = Response(500)
        message.set_status(response.status, None)
        if response.body:
            message.set_response(response.type or "application/octet-stream", Soup.MemoryUse.COPY, response.body)


class PanelClient:
    def __init__(self, timeout_s: int = 3):
        self._session = Soup.Session(timeout=timeout_s)

    def post(self, url: str, body: bytes, done: Callable[[int | None, bytes], None]) -> None:
        message = Soup.Message.new("POST", url)
        if message is None:
            GLib.idle_add(lambda: done(None, b""))
            return
        message.set_request_body_from_bytes("application/octet-stream", GLib.Bytes.new(body))
        self._session.send_and_read_async(message, GLib.PRIORITY_DEFAULT, None, self._sent, (message, done))

    @staticmethod
    def _sent(session, result, data) -> None:
        message, done = data
        try:
            got = session.send_and_read_finish(result)
        except GLib.Error:
            done(None, b"")
            return
        done(message.get_status(), got.get_data() or b"")
