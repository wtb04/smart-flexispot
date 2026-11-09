from desklink import seal

KEY_HEX = "00" * 32
KEY = bytes(32)


class FakeTimers:
    def __init__(self):
        self.now = 1000.0
        self._pending = []

    class Timer:
        def __init__(self, owner, due, fn, every):
            self.owner, self.due, self.fn, self.every, self.alive = owner, due, fn, every, True

        def cancel(self):
            self.alive = False

    def after(self, seconds, fn):
        timer = self.Timer(self, self.now + seconds, fn, None)
        self._pending.append(timer)
        return timer

    def every(self, seconds, fn):
        timer = self.Timer(self, self.now + seconds, fn, seconds)
        self._pending.append(timer)
        return timer

    def advance(self, seconds):
        end = self.now + seconds
        while True:
            due = sorted((t for t in self._pending if t.alive and t.due <= end), key=lambda t: t.due)
            if not due:
                break
            timer = due[0]
            self.now = timer.due
            if timer.every:
                timer.due += timer.every
            else:
                timer.alive = False
            timer.fn()
        self.now = end
        self._pending = [t for t in self._pending if t.alive]


class FakeClock:
    def __init__(self, timers):
        self.timers = timers

    def monotonic(self):
        return self.timers.now

    def wall_ms(self):
        return int(1_791_223_792_000 + self.timers.now * 1000)


class FakeClient:
    """Answers like the panel: a sealed pong to every state."""

    def __init__(self, clock, reaches=True, status=200, defer=False, desk=None, going=None, desk_status=200,
                 pong=None):
        self.posts = []
        self.pending = []
        self.reaches, self.status, self.defer = reaches, status, defer
        self.desk, self.going, self.desk_status, self.pong = desk, going, desk_status, pong
        self.panel = seal.Envelope(KEY, clock_ms=clock.wall_ms)

    def post(self, url, body, done):
        message = self.panel.opened(body, "/link")
        self.posts.append((url, message))
        if self.defer:
            self.pending.append(done)
            return
        if self.status is None:
            done(None, b"")
            return
        if message and message["type"] == "desk":
            if self.desk_status == 200 and self.desk is not None and "preset" in message:
                # as the screen's own buttons: any tap on its way stops it
                self.going = None if self.going else (message["preset"] if message["preset"] != self.desk else None)
            done(self.desk_status, self.panel.sealed({"type": "desk", **self.desk_fields()}, "/link reply"))
            return
        pong = self.pong or {"type": "pong", "panel": "smart-flexispot", "firmware": "v0.9.0",
                             **self.desk_fields()}
        if self.reaches is not None and self.pong is None:
            pong["reaches"] = self.reaches
        done(self.status, self.panel.sealed(pong, "/link reply"))

    def desk_fields(self):
        if self.desk is None:
            return {}
        return {"desk": self.desk, **({"going": self.going} if self.going else {})}

    def states(self):
        return [m for _, m in self.posts if m and m["type"] == "state"]

    def desks(self):
        return [m["preset"] for _, m in self.posts if m and m["type"] == "desk" and "preset" in m]

    def asked(self):
        return len([m for _, m in self.posts if m and m["type"] == "desk" and "preset" not in m])


class FakeVolume:
    def __init__(self, percent=40, muted=False):
        self.percent, self.muted, self.calls = percent, muted, []

    def set_percent(self, level):
        self.calls.append(("volume", level))

    def set_muted(self, muted):
        self.calls.append(("mute", muted))

    def step(self, delta):
        self.calls.append(("step", delta))
        return self.percent is not None


class FakeCovers:
    def __init__(self):
        self.asked = []

    def load(self, url, done):
        self.asked.append((url, done))


class FakeYouTube:
    def __init__(self):
        self.asked = []

    def find(self, title, channel, small, done):
        self.asked.append((title, channel, small, done))
