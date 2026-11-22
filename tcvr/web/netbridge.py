"""Toy Commander browser version: the LAN bridge for network play.

A web page can't send UDP or open TCP connections, so the page opens one WebSocket to the local
server (GET /net) and the server does the networking on the LAN for it. Python standard library
only, so nothing has to be installed.

Messages are binary WebSocket frames. The first byte says what it is; addresses are IPv4 (4 bytes)
and ports are big-endian u16.

  page -> server
    0x01 UDP_BIND     port              open this page's UDP socket on that port (0 = any free one)
    0x02 UDP_SEND     ip port payload   send one datagram from that socket
  server -> page
    0x81 UDP_BOUND    port ip           the socket is open: its port and this PC's LAN address
                                        (port 0 = it could not be opened)
    0x82 UDP_RECV     ip port payload   a datagram arrived on it

Each page gets its own UDP socket, so another player sees a browser player as
<this PC's LAN address>:<its port>. GGPO (the netplay) only needs that. The lobby (beacon and TCP)
comes later on the same connection.

This file is part of Flycast and is distributed under the GNU GPL v2 or later.
"""
import base64, hashlib, socket, struct, threading

WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_FRAME = 1 << 20

UDP_BIND, UDP_SEND = 0x01, 0x02
UDP_BOUND, UDP_RECV = 0x81, 0x82


def lan_address():
    """This PC's address on the LAN (the one its default route uses); no packet is sent."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))	# a documentation address: only picks the route
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def accept_key(key):
    return base64.b64encode(hashlib.sha1(key.encode("ascii") + WS_GUID).digest()).decode("ascii")


class WebSocket:
    """The server side of one WebSocket (RFC 6455), on a connected socket, after the handshake."""

    def __init__(self, rfile, wfile):
        self.rfile, self.wfile = rfile, wfile
        self.wlock = threading.Lock()
        self.closed = False

    def _read(self, n):
        b = self.rfile.read(n)
        if b is None or len(b) < n:
            raise EOFError
        return b

    def recv(self):
        """One whole message (bytes), or None when the page closed the connection."""
        data = b""
        while True:
            h = self._read(2)
            fin, op = h[0] & 0x80, h[0] & 0x0F
            masked, n = h[1] & 0x80, h[1] & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._read(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._read(8))[0]
            if n > MAX_FRAME:
                raise EOFError
            mask = self._read(4) if masked else None
            payload = self._read(n)
            if mask:
                payload = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
            if op == 0x8:		# close
                self.send(payload[:2], op=0x8)
                return None
            if op == 0x9:		# ping
                self.send(payload, op=0xA)
                continue
            if op == 0xA:		# pong
                continue
            data += payload
            if fin:
                return data

    def send(self, payload, op=0x2):
        n = len(payload)
        if n < 126:
            head = struct.pack(">BB", 0x80 | op, n)
        elif n < 65536:
            head = struct.pack(">BBH", 0x80 | op, 126, n)
        else:
            head = struct.pack(">BBQ", 0x80 | op, 127, n)
        with self.wlock:
            if self.closed:
                return
            try:
                self.wfile.write(head + payload)
                self.wfile.flush()
            except OSError:
                self.closed = True


class PageLink:
    """The networking one page does through the server."""

    def __init__(self, ws, log=print):
        self.ws = ws
        self.log = log
        self.udp = None
        self.lan_ip = lan_address()

    def run(self):
        try:
            while True:
                msg = self.ws.recv()
                if msg is None:
                    break
                if msg:
                    self.handle(msg)
        except (EOFError, OSError):
            pass
        finally:
            self.ws.closed = True
            if self.udp is not None:
                self.udp.close()

    def handle(self, msg):
        op = msg[0]
        if op == UDP_SEND and self.udp is not None and len(msg) >= 7:
            ip = socket.inet_ntoa(msg[1:5])
            port = struct.unpack(">H", msg[5:7])[0]
            try:
                self.udp.sendto(msg[7:], (ip, port))
            except OSError as e:
                self.log("net: send to %s:%d failed: %s" % (ip, port, e))
        elif op == UDP_BIND and len(msg) >= 3:
            self.bind(struct.unpack(">H", msg[1:3])[0])

    def bind(self, port):
        if self.udp is not None:
            self.udp.close()
            self.udp = None
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.bind(("0.0.0.0", port))
        except OSError as e:
            s.close()
            self.log("net: UDP port %d not available: %s" % (port, e))
            self.ws.send(struct.pack(">BH", UDP_BOUND, 0) + socket.inet_aton(self.lan_ip))
            return
        if hasattr(socket, "SIO_UDP_CONNRESET"):
            # Windows: a datagram sent to a port nobody listens on yet (a peer still starting)
            # comes back as ICMP "unreachable", and the next recvfrom then fails with
            # WinError 10054. GGPO just retries, so turn that report off.
            s.ioctl(socket.SIO_UDP_CONNRESET, False)
        self.udp = s
        bound = s.getsockname()[1]
        self.log("net: page plays on %s:%d" % (self.lan_ip, bound))
        self.ws.send(struct.pack(">BH", UDP_BOUND, bound) + socket.inet_aton(self.lan_ip))
        threading.Thread(target=self._udp_reader, args=(s,), daemon=True).start()

    def _udp_reader(self, s):
        # The socket is closed by run() when the page goes; it is read with a timeout because a
        # blocked recvfrom keeps the port in use until it returns.
        s.settimeout(0.25)
        while not self.ws.closed and self.udp is s:
            try:
                data, (ip, port) = s.recvfrom(65536)
            except (socket.timeout, ConnectionResetError):
                continue	# ConnectionResetError: see SIO_UDP_CONNRESET in bind()
            except OSError:
                return
            self.ws.send(struct.pack(">B", UDP_RECV) + socket.inet_aton(ip) + struct.pack(">H", port) + data)


def handle_upgrade(handler, log=print):
    """Called from an http.server handler for GET /net. Returns once the page has gone."""
    key = handler.headers.get("Sec-WebSocket-Key")
    if not key or "websocket" not in handler.headers.get("Upgrade", "").lower():
        handler.send_error(400)
        return
    handler.send_response(101, "Switching Protocols")
    handler.send_header("Upgrade", "websocket")
    handler.send_header("Connection", "Upgrade")
    handler.send_header("Sec-WebSocket-Accept", accept_key(key))
    handler.end_headers()
    handler.wfile.flush()
    handler.close_connection = True
    PageLink(WebSocket(handler.rfile, handler.wfile), log).run()
