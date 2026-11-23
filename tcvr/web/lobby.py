"""Toy Commander browser version: a browser player joins the LAN lobby of a Quest or PC host.

The lobby of the app (core/network/tcvr_lobby.h) is UDP beacons and a TCP connection, which a web
page cannot open. The local server does it for the page: this file speaks the same protocol as a
joiner, and the page drives it over HTTP with small JSON messages (serve.py):

  GET  /lobby/games                  the games seen on the LAN in the last 3 s
  POST /lobby/cmd?cid=ID             {"op":"query","address":A}        ask one address ("Enter address")
                                     {"op":"join","address":A,"port":P,"name":N,"version":V,"disc":HEX32}
                                     {"op":"ready","ready":true|false}
                                     {"op":"leave"}
  GET  /lobby/view?cid=ID            this page's lobby: phase, message, players, settings, my slot,
                                     the match start (peers, my index, input delay, start state id),
                                     and the UDP port the page must play on (ggpoPort)
  GET  /lobby/snap?cid=ID&id=SNAPID  the match start state the host sent

cid tells the browser players of one server apart (each tab keeps its own). The lobby connection
lives in the server, so it stays up while the page reloads into the match and back.

Browser players only join (no hosting, no host handover to them: they announce no standby port).

This file is part of Flycast and is distributed under the GNU GPL v2 or later.
"""
import socket, struct, threading, time, zlib

MAX_PLAYERS = 4
DISCOVERY_PORT = 19790
PROTOCOL = 3
NAME_MAX = 8
MAGIC = b"TCVL"
KIND_BEACON, KIND_QUERY = 1, 2
MAX_FRAME = 2048
SNAP_CHUNK = 32 * 1024
MAX_SNAP_FRAME = SNAP_CHUNK + 512
SNAP_MAX = 48 * 1024 * 1024

HELLO, WELCOME, REFUSE, STATE, READY, LEAVE, PING, START, SNAP, SNAPOK = range(1, 11)

VISIBLE_S = 3.0
PING_S = 2.0
DROP_S = 6.0
CONNECT_S = 5.0

REFUSE_TEXT = {
    1: "This game is full",
    2: "The host has a different version of the game app",
    3: "The host has a different disc",
    4: "This game has already started",
}
MODE_NAMES = ["Deathmatch", "Cat and Mice", "Capture the Flag"]


# ---- the wire format (tcvr_lobby.cpp Writer / Reader) ----------------------------------------

def clean_text(s):
    return "".join(c for c in s if 0x20 <= ord(c) < 0x7F)


def clean_name(s):
    s = clean_text(s)[:NAME_MAX]
    return s or "???"


class Writer:
    def __init__(self):
        self.b = bytearray()

    def u8(self, v):
        self.b.append(v & 0xFF)

    def u16(self, v):
        self.b += struct.pack("<H", v & 0xFFFF)

    def u32(self, v):
        self.b += struct.pack("<I", v & 0xFFFFFFFF)

    def str(self, s):
        data = s.encode("latin-1", "replace")[:255]
        self.u8(len(data))
        self.b += data

    def settings(self, s):
        self.u8(s["mode"]); self.u8(s["room"]); self.u16(s["points"])


class Reader:
    def __init__(self, data):
        self.p, self.i, self.ok = bytes(data), 0, True

    def take(self, k):
        if not self.ok or self.i + k > len(self.p):
            self.ok = False
            return False
        return True

    def u8(self):
        if not self.take(1):
            return 0
        self.i += 1
        return self.p[self.i - 1]

    def u16(self):
        if not self.take(2):
            return 0
        self.i += 2
        return struct.unpack_from("<H", self.p, self.i - 2)[0]

    def u32(self):
        lo = self.u16()
        return lo | (self.u16() << 16)

    def str(self):
        n = self.u8()
        if not self.take(n):
            return ""
        self.i += n
        return self.p[self.i - n:self.i].decode("latin-1")

    def bytes(self, k):
        if not self.take(k):
            return b""
        self.i += k
        return self.p[self.i - k:self.i]

    def settings(self):
        return {"mode": self.u8(), "room": self.u8(), "points": self.u16()}


def frame(msg_type, payload=b""):
    return struct.pack("<HB", 1 + len(payload), msg_type) + bytes(payload)


class FrameParser:
    def __init__(self):
        self.buf = bytearray()

    def feed(self, data):
        """The whole frames received so far, as (type, payload); None: the stream is broken."""
        self.buf += data
        out, pos = [], 0
        while len(self.buf) - pos >= 2:
            n = self.buf[pos] | (self.buf[pos + 1] << 8)
            if n < 1 or n > MAX_SNAP_FRAME:
                return None
            if len(self.buf) - pos >= 3:
                t = self.buf[pos + 2]
                if t < HELLO or t > SNAPOK or (n > MAX_FRAME and t != SNAP):
                    return None
            if len(self.buf) - pos < 2 + n:
                break
            out.append((self.buf[pos + 2], bytes(self.buf[pos + 3:pos + 2 + n])))
            pos += 2 + n
        del self.buf[:pos]
        return out


def encode_hello(name, version, disc, ggpo_port, lobby_port=0, lobby_id=0, slot=0xFF, ready=False):
    w = Writer()
    w.u8(PROTOCOL)
    w.str(name)
    w.str(version)
    w.b += bytes(disc)[:16].ljust(16, b"\0")
    w.u16(ggpo_port)
    w.u16(lobby_port)
    w.u32(lobby_id)
    w.u8(slot)
    w.u8(1 if ready else 0)
    return bytes(w.b)


def decode_hello(p):
    r = Reader(p)
    m = {"protocol": r.u8(), "name": clean_name(r.str()), "version": clean_text(r.str()), "disc": r.bytes(16),
         "ggpoPort": r.u16()}
    if m["protocol"] == PROTOCOL:
        m.update(lobbyPort=r.u16(), lobbyId=r.u32(), slot=r.u8(), ready=r.u8() != 0)
    return m if r.ok else None


def decode_state(p):
    r = Reader(p)
    s = {"revision": r.u32(), "settings": r.settings(), "lobbyId": r.u32(), "hostSlot": r.u8(), "players": []}
    for _ in range(MAX_PLAYERS):
        pl = {"present": r.u8() != 0}
        if pl["present"]:
            pl.update(name=clean_name(r.str()), ready=r.u8() != 0, address=clean_text(r.str()),
                      ggpoPort=r.u16(), lobbyPort=r.u16())
        s["players"].append(pl)
    return s if r.ok and s["hostSlot"] < MAX_PLAYERS else None


def encode_state(s):
    w = Writer()
    w.u32(s["revision"]); w.settings(s["settings"]); w.u32(s["lobbyId"]); w.u8(s["hostSlot"])
    for pl in s["players"]:
        w.u8(1 if pl["present"] else 0)
        if pl["present"]:
            w.str(pl["name"]); w.u8(1 if pl["ready"] else 0); w.str(pl["address"])
            w.u16(pl["ggpoPort"]); w.u16(pl["lobbyPort"])
    return bytes(w.b)


def decode_start(p):
    r = Reader(p)
    count = r.u8()
    if count < 2 or count > MAX_PLAYERS:
        return None
    peers = []
    for _ in range(count):
        peers.append({"address": clean_text(r.str()), "port": r.u16()})
    m = {"peers": peers, "localIndex": r.u8(), "inputDelay": r.u8(), "sessionId": r.u32(),
         "settings": r.settings(), "snapshot": clean_text(r.str())}
    return m if r.ok and m["localIndex"] < count else None


def encode_start(m):
    w = Writer()
    w.u8(len(m["peers"]))
    for p in m["peers"]:
        w.str(p["address"]); w.u16(p["port"])
    w.u8(m["localIndex"]); w.u8(m["inputDelay"]); w.u32(m["sessionId"]); w.settings(m["settings"]); w.str(m["snapshot"])
    return bytes(w.b)


def decode_snap(p):
    r = Reader(p)
    m = {"players": r.u8(), "id": clean_text(r.str()), "total": r.u32(), "offset": r.u32()}
    if (not r.ok or m["players"] < 2 or m["players"] > MAX_PLAYERS or not m["id"] or m["total"] == 0
            or m["total"] > SNAP_MAX or m["offset"] > m["total"] or len(p) - r.i > m["total"] - m["offset"]):
        return None
    m["bytes"] = bytes(p[r.i:])
    return m


def encode_snap(players, snap_id, total, offset, data):
    w = Writer()
    w.u8(players); w.str(snap_id); w.u32(total); w.u32(offset)
    return bytes(w.b) + bytes(data)


def encode_snapok(players, snap_id):
    w = Writer()
    w.u8(players); w.str(snap_id)
    return bytes(w.b)


def encode_beacon(b):
    w = Writer()
    w.b += MAGIC
    w.u8(KIND_BEACON); w.u8(b.get("protocol", PROTOCOL)); w.u32(b["hostId"])
    w.str(b["version"]); w.str(b["gameId"]); w.str(b["hostName"]); w.settings(b["settings"])
    w.u8(b["players"]); w.u8(b["maxPlayers"]); w.u16(b["lobbyPort"]); w.u8(1 if b["started"] else 0)
    return bytes(w.b)


def encode_query():
    return MAGIC + bytes([KIND_QUERY, PROTOCOL])


def decode_datagram(data):
    """("beacon", dict), ("query", None) or (None, None)."""
    r = Reader(data)
    if r.bytes(4) != MAGIC:
        return None, None
    kind, protocol = r.u8(), r.u8()
    if not r.ok:
        return None, None
    if kind == KIND_QUERY:
        return "query", None
    if kind != KIND_BEACON:
        return None, None
    b = {"protocol": protocol, "hostId": r.u32(), "version": clean_text(r.str()), "gameId": clean_text(r.str()),
         "hostName": clean_name(r.str()), "settings": r.settings(), "players": r.u8(), "maxPlayers": r.u8(),
         "lobbyPort": r.u16(), "started": r.u8() != 0}
    if not r.ok or b["lobbyPort"] == 0 or b["players"] > MAX_PLAYERS or b["maxPlayers"] > MAX_PLAYERS:
        return None, None
    return "beacon", b


# ---- finding games ----------------------------------------------------------------------------

class Discovery:
    """Listens to the beacons on the LAN (one listener for the whole server)."""

    def __init__(self, port=DISCOVERY_PORT, log=print):
        self.port, self.log = port, log
        self.lock = threading.Lock()
        self.seen = {}		# (hostId, address) -> (time, beacon)
        self.sock = None

    def start(self):
        if self.sock is not None:
            return True
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)		# the PC app on this PC listens there too
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        try:
            s.bind(("0.0.0.0", self.port))
        except OSError as e:
            self.log("lobby: cannot listen for games on UDP %d: %s" % (self.port, e))
            s.close()
            return False
        if hasattr(socket, "SIO_UDP_CONNRESET"):
            s.ioctl(socket.SIO_UDP_CONNRESET, False)
        self.sock = s
        threading.Thread(target=self._read, daemon=True).start()
        return True

    def _read(self):
        while True:
            try:
                data, (ip, _) = self.sock.recvfrom(4096)
            except ConnectionResetError:
                continue
            except OSError:
                return
            kind, b = decode_datagram(data)
            if kind == "beacon":
                with self.lock:
                    self.seen[(b["hostId"], ip)] = (time.monotonic(), b)

    def query(self, address):
        """Ask one address for its beacon ("Enter address")."""
        if self.sock is None and not self.start():
            return
        try:
            self.sock.sendto(encode_query(), (address, self.port))
        except OSError as e:
            self.log("lobby: query to %s failed: %s" % (address, e))

    def games(self):
        now = time.monotonic()
        with self.lock:
            for k in [k for k, (t, _) in self.seen.items() if now - t > VISIBLE_S]:
                del self.seen[k]
            items = sorted(self.seen.items(), key=lambda kv: kv[1][0])
        out, hosts = [], {}
        for (host_id, ip), (_, b) in items:
            # a host heard on several addresses (its LAN one and this PC's own): one entry, the LAN one
            if host_id in hosts:
                if ip.startswith("127."):
                    continue
                out.remove(hosts[host_id])
            g = dict(b)
            g["address"] = ip
            g["modeName"] = MODE_NAMES[min(b["settings"]["mode"], 2)]
            hosts[host_id] = g
            out.append(g)
        return out


# ---- one browser player in a lobby ------------------------------------------------------------

def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Joiner:
    """A browser player's lobby connection to a host (tcvr_lobby.cpp, the joiner side)."""

    def __init__(self, log=print, ping_s=PING_S, drop_s=DROP_S, connect_s=CONNECT_S):
        self.log = log
        self.ping_s, self.drop_s, self.connect_s = ping_s, drop_s, connect_s
        self.lock = threading.Lock()
        self.sock = None
        self.gen = 0
        self.ggpo_port = 0
        self.ready = False
        self.snaps = {}		# (players, id) -> bytes, whole
        self.arriving = {}	# players -> [id, bytearray, got]
        self._reset_view("idle", "")

    def _reset_view(self, phase, message):
        self.view = {"phase": phase, "message": message, "mySlot": -1, "state": None, "start": None,
                     "hostAddress": "", "lobbyPort": 0, "refuse": 0}

    def snapshot(self, snap_id):
        with self.lock:
            for (players, i), data in self.snaps.items():
                if i == snap_id:
                    return data
        return None

    def get_view(self):
        with self.lock:
            v = dict(self.view)
            v["ggpoPort"] = self.ggpo_port
            v["ready"] = self.ready
            v["snapshots"] = sorted(i for (_, i) in self.snaps)
        return v

    def join(self, address, port, name, version, disc):
        self.leave()
        with self.lock:
            self.gen += 1
            gen = self.gen
            self.ready = False
            self.ggpo_port = self.ggpo_port or free_udp_port()
            self._reset_view("connecting", "")
            self.view["hostAddress"], self.view["lobbyPort"] = address, port
            hello = encode_hello(clean_name(name), version, disc, self.ggpo_port)
        threading.Thread(target=self._run, args=(gen, address, port, hello), daemon=True).start()

    def match_over(self):
        """The page is back from the match: in the lobby again (not ready), still connected to the host."""
        with self.lock:
            if self.view["phase"] != "started" or self.sock is None:
                return
            self.view["phase"], self.view["start"] = "inLobby", None
        self.set_ready(False)

    def set_ready(self, ready):
        with self.lock:
            self.ready = bool(ready)
            sock = self.sock
        self._send(sock, READY, bytes([1 if ready else 0]))

    def leave(self):
        with self.lock:
            self.gen += 1
            sock, self.sock = self.sock, None
            if self.view["phase"] in ("connecting", "inLobby", "started"):
                self._reset_view("idle", "")
        if sock is not None:
            self._send(sock, LEAVE)
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()

    def _send(self, sock, msg_type, payload=b""):
        if sock is None:
            return
        try:
            with self.lock:		# one writer at a time (pings from the reader thread)
                sock.sendall(frame(msg_type, payload))
        except OSError:
            pass

    def _set(self, gen, **kw):
        with self.lock:
            if gen == self.gen:
                self.view.update(kw)
                return True
        return False

    def _run(self, gen, address, port, hello):
        try:
            sock = socket.create_connection((address, port), timeout=self.connect_s)
        except OSError as e:
            self._set(gen, phase="failed", message="The host doesn't answer")
            self.log("lobby: cannot reach %s:%d: %s" % (address, port, e))
            return
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        with self.lock:
            if gen != self.gen:
                sock.close()
                return
            self.sock = sock
        self._send(sock, HELLO, hello)
        sock.settimeout(0.25)
        parser = FrameParser()
        started_at = last_rx = last_ping = time.monotonic()
        while True:
            with self.lock:
                if gen != self.gen:
                    return
                phase = self.view["phase"]
            now = time.monotonic()
            if phase == "connecting" and now - started_at > self.connect_s:
                self._close(gen, sock, "failed", "The host doesn't answer")
                return
            if phase != "started" and now - last_rx > self.drop_s:
                self._close(gen, sock, "hostLeft", "The host left")
                return
            if now - last_ping >= self.ping_s:
                last_ping = now
                self._send(sock, PING)
            try:
                data = sock.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                data = b""
            if not data:
                self._close(gen, sock, "hostLeft", "The host left")
                return
            last_rx = now
            frames = parser.feed(data)
            if frames is None:
                self._close(gen, sock, "failed", "The host sent something this page does not understand")
                return
            for t, p in frames:
                if not self._on_frame(gen, sock, t, p):
                    return

    def _close(self, gen, sock, phase, message):
        self._set(gen, phase=phase, message=message)
        with self.lock:
            if self.sock is sock:
                self.sock = None
        try:
            sock.close()
        except OSError:
            pass

    def _on_frame(self, gen, sock, t, p):
        if t == WELCOME:
            if len(p) < 1 or p[0] >= MAX_PLAYERS:
                self._close(gen, sock, "failed", "The host sent a bad answer")
                return False
            self._set(gen, phase="inLobby", mySlot=p[0], message="")
        elif t == REFUSE:
            why = p[0] if p else 0
            self._close(gen, sock, "refused", REFUSE_TEXT.get(why, "The host refused"))
            self._set(gen, refuse=why)
            return False
        elif t == STATE:
            s = decode_state(p)
            if s is None:
                self._close(gen, sock, "failed", "The host sent a bad lobby")
                return False
            with self.lock:
                old = self.view["state"]
                if gen == self.gen and (old is None or s["revision"] >= old["revision"]):
                    self.view["state"] = s
        elif t == SNAP:
            m = decode_snap(p)
            if m is None:
                self._close(gen, sock, "failed", "The host sent a bad start state")
                return False
            self._snap_piece(sock, m)
        elif t == START:
            m = decode_start(p)
            if m is None:
                self._close(gen, sock, "failed", "The host sent a bad match start")
                return False
            self._set(gen, phase="started", start=m)
            self.log("lobby: START, player %d of %d" % (m["localIndex"] + 1, len(m["peers"])))
        elif t == LEAVE:
            self._close(gen, sock, "hostLeft", "The host left")
            return False
        return True

    def _snap_piece(self, sock, m):
        with self.lock:
            a = self.arriving.get(m["players"])
            if m["offset"] == 0 or a is None or a[0] != m["id"] or len(a[1]) != m["total"]:
                a = [m["id"], bytearray(m["total"]), 0]
                self.arriving[m["players"]] = a
            if m["offset"] != a[2]:
                del self.arriving[m["players"]]
                return
            a[1][m["offset"]:m["offset"] + len(m["bytes"])] = m["bytes"]
            a[2] += len(m["bytes"])
            if a[2] < len(a[1]):
                return
            for k in [k for k in self.snaps if k[0] == m["players"]]:
                del self.snaps[k]
            self.snaps[(m["players"], m["id"])] = bytes(a[1])
            del self.arriving[m["players"]]
        self.log("lobby: match start state %s for %d players arrived (%d bytes)" % (m["id"], m["players"], m["total"]))
        self._send(sock, SNAPOK, encode_snapok(m["players"], m["id"]))


def raw_state(data):
    """A savestate file of the app (FLYSAVE1 header, picture, RZIP chunks: core/nullDC.cpp, core/archive/rzip.cpp)
    as the raw bytes the core's Deserializer reads (the browser core has no savestate files); None if it is not one."""
    if len(data) < 24 or data[:8] != b"FLYSAVE1":
        return None
    png = struct.unpack_from("<I", data, 20)[0]
    pos = 24 + png
    if data[pos:pos + 8] != b"#RZIPv\x01#":
        return None
    pos += 8 + 4		# header, max chunk size
    size = struct.unpack_from("<Q", data, pos)[0]
    if size >> 32:
        size &= 0xFFFFFFFF	# (an older layout: a 4-byte size)
        pos += 4
    else:
        pos += 8
    out = bytearray()
    while len(out) < size and pos + 4 <= len(data):
        n = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        if n == 0:
            continue
        try:
            out += zlib.decompress(data[pos:pos + n])
        except zlib.error:
            return None
        pos += n
    return bytes(out) if len(out) == size else None


def peer_list(view):
    """network:GGPOPeers for the match START names ("address:port,..."): the host's entry is the host's address."""
    start = view.get("start") or {}
    return ",".join("%s:%d" % (p["address"] or view["hostAddress"], p["port"]) for p in start.get("peers", []))


class Lobbies:
    """All the browser players of this server, by the page's cid."""

    def __init__(self, log=print, discovery_port=DISCOVERY_PORT):
        self.log = log
        self.discovery = Discovery(discovery_port, log)
        self.players = {}
        self.lock = threading.Lock()

    def joiner(self, cid):
        with self.lock:
            j = self.players.get(cid)
            if j is None:
                j = self.players[cid] = Joiner(self.log)
            return j

    def games(self):
        self.discovery.start()
        return self.discovery.games()

    def command(self, cid, cmd):
        op = cmd.get("op")
        if op == "query":
            self.discovery.query(str(cmd.get("address", "")))
        elif op == "join":
            disc = bytes.fromhex(str(cmd.get("disc", "")))[:16]
            self.joiner(cid).join(str(cmd["address"]), int(cmd["port"]), str(cmd.get("name", "WEB")),
                                  str(cmd.get("version", "")), disc)
        elif op == "ready":
            self.joiner(cid).set_ready(bool(cmd.get("ready")))
        elif op == "matchOver":
            self.joiner(cid).match_over()
        elif op == "leave":
            self.joiner(cid).leave()
        else:
            return False
        return True
