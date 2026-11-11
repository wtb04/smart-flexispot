import unittest

from desklink import network


class NameTest(unittest.TestCase):
    def test_wifi_by_its_ssid_and_wired_by_its_connection(self):
        self.assertEqual(network.name_of(network.WIFI, "Home 1", b"Home"), "Home")
        self.assertEqual(network.name_of(network.WIFI, "Home 1", None), "Home 1")
        self.assertEqual(network.name_of(network.WIRED, "Wired connection 1", None), "Wired connection 1")
        self.assertIsNone(network.name_of("bridge", "virbr0", None))
        self.assertIsNone(network.name_of("loopback", "lo", None))

    def test_iw_names_with_its_escapes(self):
        output = "Interface wlan0\n\t\tssid Home\n\t\ttype managed\nInterface wlan1\n\t\tssid Caf\\xc3\\xa9\n"
        self.assertEqual(network.ssids_in_iw(output), {"Home", "Café"})
        self.assertEqual(network.ssids_in_iw("Interface wlan0\n\t\ttype managed\n"), frozenset())

    def test_iwd_by_each_connected_station(self):
        objects = {
            "/net/connman/iwd/0/4": {"net.connman.iwd.Station": {"State": "connected",
                                                                 "ConnectedNetwork": "/net/connman/iwd/0/4/486f6d65_psk"}},
            "/net/connman/iwd/0/4/486f6d65_psk": {"net.connman.iwd.Network": {"Name": "Home"}},
            "/net/connman/iwd/0/5": {"net.connman.iwd.Station": {"State": "disconnected"}},
        }
        self.assertEqual(network.connected_in_iwd(objects), {"Home"})
