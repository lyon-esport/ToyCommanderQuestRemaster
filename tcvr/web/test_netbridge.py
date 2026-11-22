"""Tests for netbridge.py: a page's WebSocket reaches a UDP peer on the LAN and back.

Run: python -m unittest tcvr/web/test_netbridge.py   (or python test_netbridge.py in this folder)
"""
import base64, http.server, os, socket, struct, sys, threading, unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netbridge


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        netbridge.handle_upgrade(self, log=lambda m: None)

    def log_message(self, *a):
        pass


class Client:
    """A minimal browser-side WebSocket (masked frames, as a browser sends them)."""

    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(("GET /net HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % key).encode())
        head = b""
        while b"\r\n\r\n" not in head:
            head += self.s.recv(1)
        assert b" 101 " in head.split(b"\r\n")[0], head
        assert netbridge.accept_key(key).encode() in head
        self.f = self.s.makefile("rb")

    def send(self, payload, op=0x2):
        mask = os.urandom(4)
        n = len(payload)
        head = struct.pack(">BB", 0x80 | op, 0x80 | n) if n < 126 else struct.pack(">BBH", 0x80 | op, 0x80 | 126, n)
        self.s.sendall(head + mask + bytes(b ^ mask[i & 3] for i, b in enumerate(payload)))

    def recv(self):
        h = self.f.read(2)
        n = h[1] & 0x7F
        if n == 126:
            n = struct.unpack(">H", self.f.read(2))[0]
        return h[0] & 0x0F, self.f.read(n)


class BridgeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.srv.daemon_threads = True
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()

    def test_udp_round_trip(self):
        peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        peer.bind(("127.0.0.1", 0))
        peer.settimeout(5)
        peer_port = peer.getsockname()[1]

        c = Client(self.srv.server_address[1])
        c.send(struct.pack(">BH", netbridge.UDP_BIND, 0))
        op, msg = c.recv()
        self.assertEqual(msg[0], netbridge.UDP_BOUND)
        page_port = struct.unpack(">H", msg[1:3])[0]
        self.assertNotEqual(page_port, 0)
        self.assertEqual(len(msg), 7)

        # page -> peer
        c.send(struct.pack(">B", netbridge.UDP_SEND) + socket.inet_aton("127.0.0.1") + struct.pack(">H", peer_port) + b"hello")
        data, frm = peer.recvfrom(100)
        self.assertEqual(data, b"hello")
        self.assertEqual(frm[1], page_port)

        # peer -> page, larger than a short frame
        big = bytes(range(256)) * 2
        peer.sendto(big, ("127.0.0.1", page_port))
        op, msg = c.recv()
        self.assertEqual(op, 0x2)
        self.assertEqual(msg[0], netbridge.UDP_RECV)
        self.assertEqual(socket.inet_ntoa(msg[1:5]), "127.0.0.1")
        self.assertEqual(struct.unpack(">H", msg[5:7])[0], peer_port)
        self.assertEqual(msg[7:], big)

        # ping is answered, close is answered and the UDP port is freed
        c.send(b"pi", op=0x9)
        self.assertEqual(c.recv(), (0xA, b"pi"))
        c.send(struct.pack(">H", 1000), op=0x8)
        self.assertEqual(c.recv()[0], 0x8)
        c.s.close()
        for _ in range(50):
            try:
                t = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                t.bind(("0.0.0.0", page_port))
                t.close()
                break
            except OSError:
                threading.Event().wait(0.05)
        else:
            self.fail("UDP port not released")

    def test_port_in_use(self):
        busy = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        busy.bind(("0.0.0.0", 0))
        c = Client(self.srv.server_address[1])
        c.send(struct.pack(">BH", netbridge.UDP_BIND, busy.getsockname()[1]))
        op, msg = c.recv()
        self.assertEqual(msg[:3], struct.pack(">BH", netbridge.UDP_BOUND, 0))
        busy.close()


if __name__ == "__main__":
    unittest.main()
