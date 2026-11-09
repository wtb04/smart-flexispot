import unittest

from gi.repository import GLib

from desklink import okular
from tests import fakes

INTROSPECTED = '<node><interface name="org.freedesktop.DBus.Introspectable"/><node name="okular"/>' \
               '<node name="okular2"/><node name="okularshell"/></node>'


class FakeBus:
    """Okular processes as D-Bus has them, each document's page set by the test."""

    def __init__(self):
        self.documents: dict[tuple[str, str], list] = {}  # (name, path): [page, pages, document]
        self.called = []

    def call(self, name, path, interface, method, args, reply_type, flags, timeout, cancellable, done):
        self.called.append((name, path, method))
        done(self, (name, path, method))

    def call_finish(self, result):
        name, path, method = result
        if method == "ListNames":
            return GLib.Variant("(as)", (sorted({n for n, _ in self.documents} | {"org.freedesktop.Notifications"}),))
        if method == "Introspect":
            if not any(n == name for n, _ in self.documents):
                raise GLib.Error("gone")
            nodes = "".join(f'<node name="{p[1:]}"/>' for n, p in self.documents if n == name)
            return GLib.Variant("(s)", (f"<node>{nodes}<node name=\"okularshell\"/></node>",))
        page, pages, document = self.documents[(name, path)]
        if method == "currentPage":
            return GLib.Variant("(u)", (page,))
        if method == "pages":
            return GLib.Variant("(u)", (pages,))
        if method == "currentDocument":
            return GLib.Variant("(s)", (document,))
        return GLib.Variant("()", ())


class OkularTest(unittest.TestCase):
    def setUp(self):
        self.bus = FakeBus()
        self.timers = fakes.FakeTimers()
        self.told = []
        self.okular = okular.Okular(self.told.append, self.timers, bus=self.bus)
        self.okular.start()

    def look(self):
        self.timers.advance(okular.LOOK_EVERY_S)

    def test_names(self):
        self.assertEqual(okular.deck_of("/home/w/talks/Week 3.pdf"), "Week 3.pdf")
        self.assertEqual(okular.deck_of("file:///home/w/talk.pdf"), "talk.pdf")
        self.assertEqual(okular.documents_in(INTROSPECTED), ["/okular", "/okular2"])

    def test_follows_the_document_last_touched(self):
        self.bus.documents[("org.kde.okular-10", "/okular")] = [4, 12, "/home/w/a.pdf"]
        self.look()
        self.assertEqual(self.okular.slide, okular.Slide(4, 12, "a.pdf"))
        self.bus.documents[("org.kde.okular-20", "/okular")] = [1, 7, "/home/w/b.pdf"]
        self.look()
        self.assertEqual(self.okular.slide.deck, "b.pdf", "the one just opened")
        self.look()
        self.assertEqual(self.okular.slide.deck, "b.pdf", "and still, as nothing moved")
        self.bus.documents[("org.kde.okular-10", "/okular")][0] = 5
        self.look()
        self.assertEqual(self.okular.slide, okular.Slide(5, 12, "a.pdf"), "its page turned, so it was touched")
        self.assertEqual([s.deck if s else None for s in self.told], ["a.pdf", "b.pdf", "a.pdf"])

    def test_a_tab_in_the_same_okular(self):
        self.bus.documents[("org.kde.okular-10", "/okular")] = [2, 12, "/home/w/a.pdf"]
        self.look()
        self.bus.documents[("org.kde.okular-10", "/okular2")] = [3, 9, "/home/w/c.pdf"]
        self.look()
        self.assertEqual(self.okular.slide, okular.Slide(3, 9, "c.pdf"))

    def test_closed_is_forgotten(self):
        self.bus.documents[("org.kde.okular-10", "/okular")] = [2, 12, "/home/w/a.pdf"]
        self.look()
        del self.bus.documents[("org.kde.okular-10", "/okular")]
        self.look()
        self.assertIsNone(self.okular.slide)
        self.assertEqual(self.told[-1], None)

    def test_an_empty_tab_is_no_slide(self):
        self.bus.documents[("org.kde.okular-10", "/okular")] = [0, 0, ""]
        self.look()
        self.assertIsNone(self.okular.slide)

    def test_turns_the_followed_document(self):
        self.assertFalse(self.okular.turn(True), "none open")
        self.bus.documents[("org.kde.okular-10", "/okular")] = [2, 12, "/home/w/a.pdf"]
        self.look()
        self.assertTrue(self.okular.turn(True))
        self.assertTrue(self.okular.turn(False))
        self.assertTrue(self.okular.show())
        self.assertEqual([c for c in self.bus.called if c[2].startswith("slot")],
                         [("org.kde.okular-10", "/okular", "slotNextPage"),
                          ("org.kde.okular-10", "/okular", "slotPreviousPage"),
                          ("org.kde.okular-10", "/okular", "slotTogglePresentation")])


if __name__ == "__main__":
    unittest.main()
