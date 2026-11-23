"""Tests of lobby.py (python -m unittest test_lobby). The wire format follows
tests/src/TcvrLobbyTest.cpp; lobby.py was also run against the app's own lobby (a host built from
core/network/tcvr_lobby.cpp): found, joined, start state received, START, refused on another version."""
import socket, struct, threading, time, unittest, zlib

import lobby


class Codec(unittest.TestCase):
    def test_hello(self):
        p = lobby.encode_hello("ABC", "v1.0.0-1-gabc", bytes(range(16)), 19713, 0, 0, 0xFF, False)
        m = lobby.decode_hello(p)
        self.assertEqual((m["name"], m["version"], m["disc"], m["ggpoPort"], m["slot"]),
                         ("ABC", "v1.0.0-1-gabc", bytes(range(16)), 19713, 0xFF))

    def test_state(self):
        s = {"revision": 7, "settings": {"mode": 2, "room": 5, "points": 30}, "lobbyId": 99, "hostSlot": 0,
             "players": [{"present": True, "name": "PCH", "ready": True, "address": "", "ggpoPort": 19713, "lobbyPort": 5000},
                         {"present": False}, {"present": True, "name": "WEB", "ready": False, "address": "192.0.2.5",
                                              "ggpoPort": 40000, "lobbyPort": 0}, {"present": False}]}
        self.assertEqual(lobby.decode_state(lobby.encode_state(s)), s)
        self.assertIsNone(lobby.decode_state(lobby.encode_state(s)[:-3]))

    def test_start(self):
        m = {"peers": [{"address": "", "port": 19713}, {"address": "192.0.2.5", "port": 40000}], "localIndex": 1,
             "inputDelay": 2, "sessionId": 1234, "settings": {"mode": 0, "room": 1, "points": 10}, "snapshot": "abc"}
        self.assertEqual(lobby.decode_start(lobby.encode_start(m)), m)
        v = {"start": m, "hostAddress": "192.0.2.2"}
        self.assertEqual(lobby.peer_list(v), "192.0.2.2:19713,192.0.2.5:40000")
        bad = dict(m, peers=m["peers"][:1], localIndex=0)
        self.assertIsNone(lobby.decode_start(lobby.encode_start(bad)))

    def test_beacon_and_query(self):
        b = {"hostId": 5, "version": "v", "gameId": "T-1", "hostName": "PCH", "settings": {"mode": 1, "room": 3, "points": 120},
             "players": 2, "maxPlayers": 4, "lobbyPort": 6000, "started": False}
        kind, d = lobby.decode_datagram(lobby.encode_beacon(b))
        self.assertEqual(kind, "beacon")
        self.assertEqual({k: d[k] for k in b}, b)
        self.assertEqual(lobby.decode_datagram(lobby.encode_query())[0], "query")
        self.assertEqual(lobby.decode_datagram(b"junk")[0], None)

    def test_frames(self):
        data = lobby.frame(lobby.PING) + lobby.frame(lobby.READY, b"\x01")
        fp = lobby.FrameParser()
        self.assertEqual(fp.feed(data[:2]), [])
        self.assertEqual(fp.feed(data[2:]), [(lobby.PING, b""), (lobby.READY, b"\x01")])
        self.assertIsNone(lobby.FrameParser().feed(struct.pack("<HB", 3000, lobby.STATE)))

    def test_names(self):
        self.assertEqual(lobby.clean_name("a\x01bcdefghijk"), "abcdefgh")
        self.assertEqual(lobby.clean_name("\x02"), "???")


def savestate(raw, chunk=1000, png=b"PNGDATA"):
    """A file as the app writes it (core/nullDC.cpp, core/archive/rzip.cpp)."""
    out = b"FLYSAVE1" + struct.pack("<QII", 0, 1, len(png)) + png + b"#RZIPv\x01#" + struct.pack("<IQ", chunk, len(raw))
    for i in range(0, len(raw), chunk):
        z = zlib.compress(raw[i:i + chunk])
        out += struct.pack("<I", len(z)) + z
    return out


class RawState(unittest.TestCase):
    def test_round_trip(self):
        raw = bytes((i * 31) & 255 for i in range(5000))
        self.assertEqual(lobby.raw_state(savestate(raw)), raw)

    def test_not_a_state(self):
        self.assertIsNone(lobby.raw_state(b"hello"))
        self.assertIsNone(lobby.raw_state(savestate(b"x" * 100)[:-5]))


class FakeHost:
    """The host side of the protocol, as far as a joiner sees it."""

    def __init__(self, refuse=0):
        self.srv = socket.socket()
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]
        self.refuse = refuse
        self.got = []
        threading.Thread(target=self.run, daemon=True).start()

    def send(self, t, p=b""):
        self.c.sendall(lobby.frame(t, p))

    def run(self):
        self.c, _ = self.srv.accept()
        fp = lobby.FrameParser()
        while True:
            try:
                data = self.c.recv(65536)
            except OSError:
                return
            if not data:
                return
            for t, p in fp.feed(data):
                self.got.append(t)
                if t == lobby.HELLO:
                    if self.refuse:
                        self.send(lobby.REFUSE, bytes([self.refuse]))
                        continue
                    self.hello = lobby.decode_hello(p)
                    self.send(lobby.WELCOME, b"\x01")
                    st = {"revision": 1, "settings": {"mode": 0, "room": 2, "points": 10}, "lobbyId": 3, "hostSlot": 0,
                          "players": [{"present": True, "name": "HST", "ready": True, "address": "", "ggpoPort": 19713, "lobbyPort": self.port},
                                      {"present": True, "name": self.hello["name"], "ready": False, "address": "127.0.0.1",
                                       "ggpoPort": self.hello["ggpoPort"], "lobbyPort": 0}, {"present": False}, {"present": False}]}
                    self.send(lobby.STATE, lobby.encode_state(st))
                    snap = bytes(range(256)) * 300
                    for off in range(0, len(snap), lobby.SNAP_CHUNK):
                        self.send(lobby.SNAP, lobby.encode_snap(2, "s1", len(snap), off, snap[off:off + lobby.SNAP_CHUNK]))
                elif t == lobby.READY and p == b"\x01":
                    self.send(lobby.START, lobby.encode_start({"peers": [{"address": "", "port": 19713},
                              {"address": "127.0.0.1", "port": self.hello["ggpoPort"]}], "localIndex": 1, "inputDelay": 2,
                              "sessionId": 9, "settings": {"mode": 0, "room": 2, "points": 10}, "snapshot": "s1"}))


def wait(f, s=5):
    end = time.time() + s
    while time.time() < end:
        if f():
            return True
        time.sleep(0.05)
    return False


class Joining(unittest.TestCase):
    def test_join_ready_start(self):
        h = FakeHost()
        j = lobby.Joiner(log=lambda m: None)
        j.join("127.0.0.1", h.port, "web", "v", bytes(16))
        self.assertTrue(wait(lambda: j.get_view()["phase"] == "inLobby" and j.get_view()["snapshots"] == ["s1"]))
        self.assertEqual(j.snapshot("s1"), bytes(range(256)) * 300)
        self.assertIn(lobby.SNAPOK, h.got)
        self.assertEqual(j.get_view()["mySlot"], 1)
        j.set_ready(True)
        self.assertTrue(wait(lambda: j.get_view()["phase"] == "started"))
        v = j.get_view()
        self.assertEqual(lobby.peer_list(v), "127.0.0.1:19713,127.0.0.1:%d" % v["ggpoPort"])
        j.match_over()
        self.assertEqual(j.get_view()["phase"], "inLobby")
        j.leave()
        self.assertTrue(wait(lambda: lobby.LEAVE in h.got))

    def test_refused(self):
        h = FakeHost(refuse=2)
        j = lobby.Joiner(log=lambda m: None)
        j.join("127.0.0.1", h.port, "web", "v", bytes(16))
        self.assertTrue(wait(lambda: j.get_view()["phase"] == "refused"))
        self.assertEqual(j.get_view()["message"], lobby.REFUSE_TEXT[2])

    def test_host_silent(self):
        h = FakeHost()
        j = lobby.Joiner(log=lambda m: None, drop_s=0.5)
        j.join("127.0.0.1", h.port, "web", "v", bytes(16))
        self.assertTrue(wait(lambda: j.get_view()["phase"] == "inLobby"))
        self.assertTrue(wait(lambda: j.get_view()["phase"] == "hostLeft"))


if __name__ == "__main__":
    unittest.main()
