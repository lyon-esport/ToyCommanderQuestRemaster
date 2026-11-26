#!/usr/bin/env python3
"""Tests of tcvr_host.py (the server app and its admin page) that need no disc and no tcvr-server."""
import json, os, socket, ssl, sys, tempfile, time, unittest, urllib.error, urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tcvr_host


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class HostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        tcvr_host.CONFIG_PATH = os.path.join(cls.tmp, "tcvr-host.json")
        tcvr_host.PASSWORD_PATH = os.path.join(cls.tmp, "admin-password.txt")
        tcvr_host.LOG_DIR = os.path.join(cls.tmp, "logs")
        tcvr_host.STATUS_PATH = os.path.join(cls.tmp, "logs", "status.json")
        cfg = tcvr_host.load_config()
        cfg["admin"]["port"] = free_port()
        cfg["mp"]["folder"] = cls.tmp		# no tcvr-server there
        cls.host = tcvr_host.Host(cfg, {"admin", "mp"})
        cls.host.start()
        cls.base = "%s://127.0.0.1:%d" % (cls.host.services["admin"].scheme, cfg["admin"]["port"])
        cls.ctx = ssl._create_unverified_context()

    @classmethod
    def tearDownClass(cls):
        cls.host.stop()

    def call(self, path, body=None, cookie="", header=True):
        req = urllib.request.Request(self.base + path, data=None if body is None else json.dumps(body).encode())
        if body is not None and header:
            req.add_header("X-TCVR-Admin", "1")
        if cookie:
            req.add_header("Cookie", cookie)
        try:
            with urllib.request.urlopen(req, context=self.ctx, timeout=10) as r:
                return r.status, json.loads(r.read() or b"null"), r.headers.get("Set-Cookie", "")
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"null"), ""

    def login(self):
        code, j, cookie = self.call("/api/login", {"password": self.host.password})
        self.assertEqual(code, 200)
        return cookie.split(";")[0]

    def test_password_needed(self):
        self.assertEqual(self.call("/api/state")[0], 401)
        self.assertEqual(self.call("/api/login", {"password": "nope"})[0], 403)
        self.assertEqual(self.call("/api/login", {"password": self.host.password}, header=False)[0], 400)

    def test_state(self):
        cookie = self.login()
        code, st, _ = self.call("/api/state", cookie=cookie)
        self.assertEqual(code, 200)
        mp = [s for s in st["services"] if s["id"] == "mp"][0]
        self.assertFalse(mp["running"])
        self.assertIn("tcvr-server not found", mp["error"])
        self.assertIn("New-NetFirewallRule", mp["firewall"])

    def test_settings(self):
        cookie = self.login()
        bad = self.call("/api/settings", {"mp": {"port": 80}}, cookie)[1]
        self.assertFalse(bad["ok"])
        same = self.host.cfg["admin"]["port"]
        dup = self.call("/api/settings", {"mp": {"port": same}}, cookie)[1]
        self.assertFalse(dup["ok"])
        self.assertFalse(self.call("/api/settings", {"admin": {"allow": "any"}}, cookie)[1]["ok"])
        ok = self.call("/api/settings", {"mp": {"port": 19877, "games": "private"}}, cookie)[1]
        self.assertTrue(ok["ok"], ok)
        with open(tcvr_host.CONFIG_PATH) as f:
            self.assertEqual(json.load(f)["mp"]["port"], 19877)

    def test_lan_only(self):
        a = self.host.services["admin"]
        for ip in ("127.0.0.1", "192.168.1.20", "10.0.0.5", "::1", "::ffff:192.168.0.3"):	# leakscan:allow (sample LAN addresses)
            self.assertTrue(a.allowed(ip), ip)
        for ip in ("8.8.8.8", "2001:4860::1", "garbage"):
            self.assertFalse(a.allowed(ip), ip)

    def test_beacon(self):
        b = tcvr_host.decode_beacon("5443564c010313baed8909696e65742d7465737404544553540341414100030a0001040da700")
        self.assertEqual(b["hostName"], "AAA")
        self.assertEqual(b["modeName"], "Deathmatch")
        self.assertIsNone(tcvr_host.decode_beacon("zz"))


if __name__ == "__main__":
    unittest.main()
