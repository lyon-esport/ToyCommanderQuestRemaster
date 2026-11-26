#!/usr/bin/env python3
"""Toy Commander server app: one launcher for everything this computer can serve.

Services (choose with --services, default: all three):
  mp     internet multiplayer hosting: runs tcvr-server (headsets and PCs playing over the internet)
  web    play in a web browser: the page, the emulator and your disc for the devices on your network (serve.py)
  admin  the admin page: connection logs, running games, connected players and computers, every option,
         and the port of each service (changed there, the service restarts on the new port)

Usage:
  python tcvr_host.py                          everything
  python tcvr_host.py --services mp,admin      internet hosting and the admin page only
  python tcvr_host.py --services web           only the browser version

The settings (ports and the multiplayer server's options) live in tcvr-host.json next to this file;
the admin page edits them. The admin password is in admin-password.txt next to this file (made on the
first start). The admin page answers only this computer and the devices of the local network, unless
tcvr-host.json says "allow": "any".
"""
import argparse, collections, hmac, http.server, ipaddress, json, os, re, secrets
import socket, ssl, subprocess, sys, threading, time, urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import lobby, netbridge, serve	# noqa: E402  (the browser version's server, used as a module)

SERVICES = ("mp", "web", "admin")
SERVICE_NAMES = {"mp": "Internet multiplayer server", "web": "Play in a web browser", "admin": "Admin page"}
CONFIG_PATH = os.path.join(HERE, "tcvr-host.json")
PASSWORD_PATH = os.path.join(HERE, "admin-password.txt")
LOG_DIR = os.path.join(HERE, "logs")
STATUS_PATH = os.path.join(HERE, "logs", "tcvr-server-status.json")
ADMIN_PAGE = os.path.join(HERE, "admin", "index.html")

DEFAULTS = {
    "mp": {"port": 19800, "folder": "", "max_clients": 200, "relay_kbps": 256, "games": "all", "verbose": False},
    "web": {"port": 8642, "https_port": 8643, "listen": "0.0.0.0", "disc": "ToyCommander.chd"},
    "admin": {"port": 8644, "allow": "lan"},
}

# ---- log -------------------------------------------------------------------------------------------------

class Log:
    """Every service's lines: kept in memory for the admin page, printed, and written to logs/."""

    def __init__(self, keep=5000):
        self.lines = collections.deque(maxlen=keep)
        self.seq = 0
        self.lock = threading.Lock()

    def add(self, source, text):
        text = text.rstrip()
        if not text:
            return
        now = time.time()
        with self.lock:
            self.seq += 1
            self.lines.append({"seq": self.seq, "t": now, "source": source, "text": text})
            stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(now))
            line = "%s  [%s] %s" % (stamp, source, text)
            try:
                print(line, flush=True)
            except (OSError, UnicodeEncodeError):
                pass
            try:
                os.makedirs(LOG_DIR, exist_ok=True)
                with open(os.path.join(LOG_DIR, "tcvr-host-%s.log" % time.strftime("%Y-%m-%d", time.localtime(now))),
                          "a", encoding="utf-8") as f:
                    f.write(line + "\n")
            except OSError:
                pass

    def since(self, seq, limit=1000):
        with self.lock:
            out = [l for l in self.lines if l["seq"] > seq]
        return out[-limit:]


LOG = Log()

def logger(source):
    return lambda text: LOG.add(source, str(text))

# ---- settings --------------------------------------------------------------------------------------------

def load_config():
    cfg = json.loads(json.dumps(DEFAULTS))
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            saved = json.load(f)
        for section, values in saved.items():
            if section in cfg and isinstance(values, dict):
                for k, v in values.items():
                    if k in cfg[section]:
                        cfg[section][k] = v
    except FileNotFoundError:
        pass
    except (OSError, ValueError) as e:
        LOG.add("host", "tcvr-host.json can't be read (%s): using the defaults" % e)
    # the start script's older setting: the folder of tcvr-server on the first line of internet-server.txt
    if not cfg["mp"]["folder"]:
        old = os.path.join(HERE, "internet-server.txt")
        if os.path.isfile(old):
            with open(old, "r", encoding="utf-8-sig") as f:
                line = f.readline().strip().strip('"')
            if line.lower().endswith(".exe"):
                line = os.path.dirname(line)
            cfg["mp"]["folder"] = line
    return cfg

def save_config(cfg):
    tmp = CONFIG_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2)
    os.replace(tmp, CONFIG_PATH)

def admin_password():
    """The admin password: made once (easy to type), kept in admin-password.txt."""
    try:
        with open(PASSWORD_PATH, "r", encoding="utf-8-sig") as f:
            pw = f.readline().strip()
        if pw:
            return pw, False
    except FileNotFoundError:
        pass
    words = "".join(secrets.choice("abcdefghjkmnpqrstuvwxyz23456789") for _ in range(12))
    pw = "-".join(words[i:i + 4] for i in range(0, 12, 4))
    set_admin_password(pw)
    return pw, True

def set_admin_password(pw):
    with open(PASSWORD_PATH, "w", encoding="utf-8") as f:
        f.write(pw + "\n")
    try:
        os.chmod(PASSWORD_PATH, 0o600)
    except OSError:
        pass

def resolve(path):
    path = os.path.expandvars(os.path.expanduser(path or ""))
    return path if not path or os.path.isabs(path) else os.path.join(HERE, path)

def firewall_command(service, ports):
    """The PowerShell (administrator) line that lets the devices reach these ports."""
    parts = []
    for proto, port in ports:
        name = "Toy Commander %s %s %d" % (service, proto, port)
        parts.append("New-NetFirewallRule -DisplayName '%s' -Direction Inbound -Action Allow -Protocol %s -LocalPort %d -Profile Any"
                     % (name, proto, port))
    return "; ".join(parts)

def service_ports(cfg, name):
    if name == "mp":
        return [("TCP", cfg["mp"]["port"]), ("UDP", cfg["mp"]["port"])]
    if name == "web":
        p = [("TCP", cfg["web"]["port"])]
        if cfg["web"]["https_port"]:
            p.append(("TCP", cfg["web"]["https_port"]))
        p.append(("UDP", lobby.DISCOVERY_PORT))
        return p
    return [("TCP", cfg["admin"]["port"])]

def port_free(proto, port, listen="0.0.0.0"):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM if proto == "TCP" else socket.SOCK_DGRAM)
    try:
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            s.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        s.bind((listen, port))
        return True
    except OSError:
        return False
    finally:
        s.close()

# ---- Windows: the multiplayer server stops with this app, even when its window is closed -------------

class KillWithMe:
    def __init__(self):
        self.job = None
        if os.name != "nt":
            return
        try:
            import ctypes
            from ctypes import wintypes
            k32 = ctypes.WinDLL("kernel32", use_last_error=True)
            k32.CreateJobObjectW.restype = wintypes.HANDLE
            k32.OpenProcess.restype = wintypes.HANDLE
            job = k32.CreateJobObjectW(None, None)

            class BASIC(ctypes.Structure):
                _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64),
                            ("LimitFlags", wintypes.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                            ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                            ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD),
                            ("SchedulingClass", wintypes.DWORD)]

            class IO(ctypes.Structure):
                _fields_ = [(n, ctypes.c_uint64) for n in ("r", "w", "o", "rb", "wb", "ob")]

            class EXT(ctypes.Structure):
                _fields_ = [("Basic", BASIC), ("Io", IO), ("ProcessMemoryLimit", ctypes.c_size_t),
                            ("JobMemoryLimit", ctypes.c_size_t), ("PeakProcessMemoryUsed", ctypes.c_size_t),
                            ("PeakJobMemoryUsed", ctypes.c_size_t)]
            info = EXT()
            info.Basic.LimitFlags = 0x2000	# JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            if job and k32.SetInformationJobObject(job, 9, ctypes.byref(info), ctypes.sizeof(info)):
                self.job, self.k32 = job, k32
        except Exception:	# best effort
            self.job = None

    def add(self, proc):
        if self.job is None:
            return
        try:
            h = self.k32.OpenProcess(0x0101, False, proc.pid)	# PROCESS_TERMINATE | PROCESS_SET_QUOTA
            if h:
                self.k32.AssignProcessToJobObject(self.job, h)
                self.k32.CloseHandle(h)
        except Exception:
            pass

KILL_WITH_ME = KillWithMe()

# ---- service: internet multiplayer server (tcvr-server) -------------------------------------------------

class MpService:
    name = "mp"

    def __init__(self, host):
        self.host = host
        self.proc = None
        self.error = ""
        self.started_at = 0
        self.lock = threading.Lock()
        self.log = logger("mp")

    def folder(self):
        f = self.host.cfg["mp"]["folder"]
        return resolve(f) if f else os.path.join(HERE, "tcvr-server")

    def exe(self):
        return os.path.join(self.folder(), "tcvr-server.exe" if os.name == "nt" else "tcvr-server")

    def running(self):
        return self.proc is not None and self.proc.poll() is None

    def start(self):
        with self.lock:
            if self.running():
                return True
            c = self.host.cfg["mp"]
            exe = self.exe()
            if not os.path.isfile(exe):
                self.error = "tcvr-server not found: put it in %s (or set its folder in the settings)" % self.folder()
                self.log(self.error)
                return False
            try:
                os.remove(STATUS_PATH)
            except OSError:
                pass
            os.makedirs(LOG_DIR, exist_ok=True)
            args = [exe, "--port", str(c["port"]), "--key", os.path.join(self.folder(), "tcvr-server.key"),
                    "--max-clients", str(c["max_clients"]), "--relay-kbps", str(c["relay_kbps"]),
                    "--games", c["games"], "--status", STATUS_PATH]
            if c["verbose"]:
                args.append("--verbose")
            flags = 0x08000000 if os.name == "nt" else 0	# CREATE_NO_WINDOW: its lines show in this window
            try:
                self.proc = subprocess.Popen(args, cwd=self.folder(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                             stdin=subprocess.DEVNULL, creationflags=flags)
            except OSError as e:
                self.error = "tcvr-server can't start: %s" % e
                self.log(self.error)
                return False
            KILL_WITH_ME.add(self.proc)
            self.error = ""
            self.started_at = time.time()
            threading.Thread(target=self._read, args=(self.proc,), daemon=True).start()
        # it stops at once when the port is taken
        time.sleep(0.6)
        if not self.running():
            self.error = self.error or "tcvr-server stopped right away (is the port %d taken by another program?)" % self.host.cfg["mp"]["port"]
            return False
        return True

    def _read(self, proc):
        for raw in proc.stdout:
            text = raw.decode("utf-8", "replace").rstrip()
            # its own lines start with the date and time: this log adds them
            text = re.sub(r"^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d  ", "", text)
            self.log(text)
        code = proc.wait()
        if proc is self.proc and not getattr(proc, "stopping", False):
            self.error = "tcvr-server stopped (exit code %d)" % code
            self.log(self.error)

    def stop(self):
        with self.lock:
            p = self.proc
            if p is None or p.poll() is not None:
                return
            p.stopping = True
            p.terminate()
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
            self.log("tcvr-server stopped")

    def status(self):
        """What tcvr-server wrote in its status file (games and players), when it runs."""
        if not self.running():
            return None
        try:
            with open(STATUS_PATH, "r", encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return None

    def addresses(self):
        p = self.host.cfg["mp"]["port"]
        return ["%s%s" % (ip, "" if p == 19800 else ":%d" % p) for ip in serve.lan_addresses()]

# ---- service: play in a web browser (serve.py's web server, in this process) -----------------------------

class WebClients:
    """The browsers that use the web server: by address."""

    def __init__(self):
        self.lock = threading.Lock()
        self.by_ip = {}
        self.log = logger("web")
        self.current = threading.local()	# leakscan:allow (a Python class, not a hostname)

    def seen(self, ip, path, agent):
        now = time.time()
        with self.lock:
            c = self.by_ip.get(ip)
            fresh = c is None or now - c["last"] > 600
            if c is None:
                c = self.by_ip[ip] = {"address": ip, "first": now, "last": now, "requests": 0, "links": 0,
                                      "agent": "", "pages": 0, "cids": {}}
            c["last"] = now
            c["requests"] += 1
            if agent:
                c["agent"] = browser_name(agent)
            if path in ("/", "/play.html", "/index.html"):
                c["pages"] += 1
        if fresh and path in ("/", "/play.html", "/index.html"):
            self.log("%s opened the game page (%s)" % (ip, c["agent"] or "browser"))

    def link(self, ip, delta):
        with self.lock:
            c = self.by_ip.get(ip)
            if c is not None:
                c["links"] = max(0, c["links"] + delta)
        self.log("%s network link %s" % (ip, "opened (the page plays online)" if delta > 0 else "closed"))

    def command(self, ip, cid, cmd):
        op = cmd.get("op")
        with self.lock:
            c = self.by_ip.get(ip)
            if c is not None and cid:
                c["cids"].setdefault(cid, {"name": ""})
                if cmd.get("name"):
                    c["cids"][cid]["name"] = lobby.clean_name(str(cmd.get("name")))
                name = c["cids"][cid]["name"]
            else:
                name = ""
        if op == "host":
            self.log("%s %screates a game" % (ip, name + " " if name else ""))
        elif op == "join":
            self.log("%s %sjoins the game at %s" % (ip, name + " " if name else "", cmd.get("address", "?")))
        elif op == "start":
            self.log("%s %sstarts the match" % (ip, name + " " if name else ""))
        elif op == "leave":
            self.log("%s %sleaves the lobby" % (ip, name + " " if name else ""))

    def snapshot(self):
        with self.lock:
            out = []
            for c in self.by_ip.values():
                d = dict(c)
                d["cids"] = {k: dict(v) for k, v in c["cids"].items()}
                out.append(d)
        return out


def browser_name(agent):
    for key, name in (("Edg/", "Edge"), ("OPR/", "Opera"), ("SamsungBrowser", "Samsung Internet"),
                      ("OculusBrowser", "Quest browser"), ("Firefox/", "Firefox"), ("Chrome/", "Chrome"),
                      ("Safari/", "Safari")):
        if key in agent:
            break
    else:
        name = "browser"
    for key, system in (("Android", "Android"), ("iPad", "iPad"), ("iPhone", "iPhone"), ("Windows", "Windows"),
                        ("Mac OS", "Mac"), ("Linux", "Linux")):
        if key in agent:
            return "%s, %s" % (name, system)
    return name


class TrackedHandler(serve.Handler):
    """serve.py's handler, telling the admin page who uses it."""
    clients = None

    def handle_one_request(self):
        try:
            super().handle_one_request()
        except (ConnectionError, ssl.SSLError):
            pass

    def _note(self):
        ip = self.client_address[0]
        if ip.startswith("::ffff:"):
            ip = ip[7:]
        self.clients.seen(ip, urllib.parse.urlparse(self.path).path, self.headers.get("User-Agent", ""))
        self.clients.current.ip = ip
        return ip

    def do_GET(self):
        ip = self._note()
        if urllib.parse.urlparse(self.path).path == "/net":
            self.clients.link(ip, +1)
            try:
                netbridge.handle_upgrade(self, log=logger("web"))
            finally:
                self.clients.link(ip, -1)
            return
        super().do_GET()

    def do_POST(self):
        self._note()
        super().do_POST()

    def log_message(self, fmt, *args):
        pass	# the admin page's log has what matters


def wrap_lobby_commands(clients):
    original = serve.LOBBIES.command

    def command(cid, cmd):
        try:
            clients.command(getattr(clients.current, "ip", "?"), cid, cmd)
        except Exception:
            pass
        return original(cid, cmd)
    serve.LOBBIES.command = command
    serve.LOBBIES.log = logger("web")
    serve.LOBBIES.discovery.log = logger("web")


class ThreadingServer(http.server.ThreadingHTTPServer):
    request_queue_size = 128
    daemon_threads = True
    allow_reuse_address = os.name != "nt"	# on Windows it would let two servers share a port


class WebService:
    name = "web"

    def __init__(self, host):
        self.host = host
        self.servers = []
        self.error = ""
        self.started_at = 0
        self.lock = threading.Lock()
        self.log = logger("web")
        self.clients = WebClients()
        TrackedHandler.clients = self.clients
        wrap_lobby_commands(self.clients)

    def disc(self):
        return resolve(self.host.cfg["web"]["disc"])

    def running(self):
        return bool(self.servers)

    def start(self):
        with self.lock:
            if self.servers:
                return True
            c = self.host.cfg["web"]
            disc = self.disc()
            if not os.path.isfile(disc):
                self.error = "Put your own Toy Commander (Europe) disc image here: %s" % disc
                self.log(self.error)
                return False
            if not os.path.isdir(serve.ROOT):
                self.error = "The game files (www folder) are missing next to serve.py: %s" % serve.ROOT
                self.log(self.error)
                return False
            serve.Handler.disc = disc
            servers = []
            try:
                servers.append(("http", ThreadingServer((c["listen"], c["port"]), TrackedHandler)))
                if c["https_port"]:
                    made = self.host.certificate()
                    if made is None:
                        self.log("No HTTPS (gamepads on tablets and phones need it): run  python -m pip install cryptography  and start again")
                    else:
                        tls = ThreadingServer((c["listen"], c["https_port"]), TrackedHandler)
                        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                        ctx.load_cert_chain(*made)
                        tls.socket = ctx.wrap_socket(tls.socket, server_side=True)
                        servers.append(("https", tls))
            except OSError as e:
                for _, s in servers:
                    s.server_close()
                self.error = "The web server can't use its port (%s): another program may use it" % e
                self.log(self.error)
                return False
            for _, s in servers:
                threading.Thread(target=s.serve_forever, daemon=True).start()
            self.servers = servers
            self.error = ""
            self.started_at = time.time()
        for url in self.urls():
            self.log("Players open: " + url)
        return True

    def stop(self):
        with self.lock:
            servers, self.servers = self.servers, []
        for _, s in servers:
            s.shutdown()
            s.server_close()
        if servers:
            self.log("web server stopped")

    def urls(self):
        c = self.host.cfg["web"]
        ips = serve.lan_addresses() if c["listen"] == "0.0.0.0" else [c["listen"]]
        out = []
        for ip in ips or ["localhost"]:
            out.append("http://%s:%d/play.html" % (ip, c["port"]))
            if c["https_port"] and any(k == "https" for k, _ in self.servers):
                out.append("https://%s:%d/play.html" % (ip, c["https_port"]))
        return out

# ---- service: admin page --------------------------------------------------------------------------------

class AdminService:
    name = "admin"

    def __init__(self, host):
        self.host = host
        self.server = None
        self.scheme = "http"
        self.error = ""
        self.started_at = 0
        self.lock = threading.Lock()
        self.log = logger("admin")
        self.sessions = {}		# token -> expiry
        self.failures = {}		# ip -> [times]

    def running(self):
        return self.server is not None

    def start(self):
        with self.lock:
            if self.server is not None:
                return True
            c = self.host.cfg["admin"]
            handler = type("AdminHandler", (AdminHandler,), {"admin": self})
            try:
                srv = ThreadingServer(("0.0.0.0", c["port"]), handler)
            except OSError as e:
                self.error = "The admin page can't use port %d (%s)" % (c["port"], e)
                self.log(self.error)
                return False
            made = self.host.certificate()
            self.scheme = "http"
            if made is not None:
                ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                ctx.load_cert_chain(*made)
                srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
                self.scheme = "https"
            threading.Thread(target=srv.serve_forever, daemon=True).start()
            self.server = srv
            self.error = ""
            self.started_at = time.time()
        for url in self.urls():
            self.log("Admin page: " + url)
        return True

    def stop(self):
        with self.lock:
            srv, self.server = self.server, None
        if srv is not None:
            srv.shutdown()
            srv.server_close()

    def urls(self):
        p = self.host.cfg["admin"]["port"]
        return ["%s://localhost:%d/" % (self.scheme, p)] + ["%s://%s:%d/" % (self.scheme, ip, p) for ip in serve.lan_addresses()]

    def allowed(self, ip):
        if self.host.cfg["admin"]["allow"] == "any":
            return True
        try:
            a = ipaddress.ip_address(ip)
        except ValueError:
            return False
        if a.version == 6 and a.ipv4_mapped:
            a = a.ipv4_mapped
        return a.is_loopback or a.is_private or a.is_link_local

    def login(self, ip, password):
        now = time.time()
        recent = [t for t in self.failures.get(ip, []) if now - t < 60]
        if len(recent) >= 10:
            return None, "Too many wrong passwords: wait a minute"
        if not hmac.compare_digest(password.encode("utf-8"), self.host.password.encode("utf-8")):
            recent.append(now)
            self.failures[ip] = recent
            self.log("%s: wrong admin password" % ip)
            time.sleep(1.0)
            return None, "Wrong password"
        token = secrets.token_urlsafe(32)
        self.sessions[token] = now + 12 * 3600
        self.log("%s signed in to the admin page" % ip)
        return token, ""

    def session_ok(self, token):
        exp = self.sessions.get(token or "")
        if exp is None:
            return False
        if exp < time.time():
            del self.sessions[token]
            return False
        return True


class AdminHandler(http.server.BaseHTTPRequestHandler):
    admin = None
    server_version = "tcvr-host"
    sys_version = ""

    def log_message(self, fmt, *args):
        pass

    def handle_one_request(self):
        try:
            super().handle_one_request()
        except (ConnectionError, ssl.SSLError):
            pass

    def ip(self):
        ip = self.client_address[0]
        return ip[7:] if ip.startswith("::ffff:") else ip

    def send_body(self, code, body, ctype, extra=()):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Content-Security-Policy", "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; frame-ancestors 'none'")
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, obj, code=200, extra=()):
        self.send_body(code, json.dumps(obj).encode("utf-8"), "application/json", extra)

    def token(self):
        for part in self.headers.get("Cookie", "").split(";"):
            k, _, v = part.strip().partition("=")
            if k == "tcvr_admin":
                return v
        return ""

    def gate(self):
        if not self.admin.allowed(self.ip()):
            self.send_body(403, b"The admin page answers only this computer and the local network.", "text/plain")
            return False
        return True

    def read_json(self):
        n = int(self.headers.get("Content-Length", "0") or 0)
        if n > 65536:
            raise ValueError("too big")
        return json.loads(self.rfile.read(n).decode("utf-8") or "{}")

    def do_GET(self):
        if not self.gate():
            return
        path = urllib.parse.urlparse(self.path).path
        if path in ("/", "/index.html"):
            try:
                with open(ADMIN_PAGE, "rb") as f:
                    body = f.read()
            except OSError:
                body = b"admin/index.html is missing next to tcvr_host.py"
            self.send_body(200, body, "text/html; charset=utf-8")
            return
        if not path.startswith("/api/"):
            self.send_body(404, b"not found", "text/plain")
            return
        if not self.admin.session_ok(self.token()):
            self.send_json({"error": "signin"}, 401)
            return
        q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        if path == "/api/state":
            self.send_json(self.admin.host.state())
        elif path == "/api/log":
            after = int(q.get("after", ["0"])[0] or 0)
            self.send_json({"lines": LOG.since(after)})
        else:
            self.send_json({"error": "not found"}, 404)

    def do_POST(self):
        if not self.gate():
            return
        path = urllib.parse.urlparse(self.path).path
        # a page from another site can't send this header without the browser asking first (no CORS here)
        if self.headers.get("X-TCVR-Admin") != "1":
            self.send_json({"error": "bad request"}, 400)
            return
        try:
            body = self.read_json()
        except (ValueError, UnicodeDecodeError):
            self.send_json({"error": "bad request"}, 400)
            return
        secure = "; Secure" if self.admin.scheme == "https" else ""
        if path == "/api/login":
            token, why = self.admin.login(self.ip(), str(body.get("password", "")))
            if token is None:
                self.send_json({"ok": False, "error": why}, 403)
            else:
                self.send_json({"ok": True}, extra=[("Set-Cookie", "tcvr_admin=%s; Path=/; HttpOnly; SameSite=Strict%s" % (token, secure))])
            return
        token = self.token()
        if not self.admin.session_ok(token):
            self.send_json({"error": "signin"}, 401)
            return
        if path == "/api/logout":
            self.admin.sessions.pop(token, None)
            self.send_json({"ok": True}, extra=[("Set-Cookie", "tcvr_admin=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict%s" % secure)])
        elif path == "/api/settings":
            self.send_json(self.admin.host.apply_settings(body, self.ip()))
        elif path == "/api/service":
            self.send_json(self.admin.host.service_action(str(body.get("service", "")), str(body.get("action", "")), self.ip()))
        elif path == "/api/password":
            pw = str(body.get("password", ""))
            if len(pw) < 8:
                self.send_json({"ok": False, "error": "At least 8 characters"})
                return
            self.admin.host.password = pw
            set_admin_password(pw)
            self.admin.sessions = {token: self.admin.sessions.get(token, time.time() + 3600)}
            self.admin.log("%s changed the admin password" % self.ip())
            self.send_json({"ok": True})
        else:
            self.send_json({"error": "not found"}, 404)

# ---- the app ---------------------------------------------------------------------------------------------

class Host:
    def __init__(self, cfg, enabled):
        self.cfg = cfg
        self.enabled = enabled
        self.started = time.time()
        self.password, self.new_password = admin_password()
        self.services = {"mp": MpService(self), "web": WebService(self), "admin": AdminService(self)}
        self.cert_lock = threading.Lock()

    def certificate(self):
        with self.cert_lock:
            return serve.make_certificate(os.path.join(HERE, "cert"), serve.lan_addresses())

    def start(self):
        for name in SERVICES:
            if name in self.enabled:
                self.services[name].start()

    def stop(self):
        for name in reversed(SERVICES):
            self.services[name].stop()

    # -- what the admin page shows

    def state(self):
        mp, web, admin = self.services["mp"], self.services["web"], self.services["admin"]
        st = mp.status()
        services = []
        for name in SERVICES:
            s = self.services[name]
            item = {"id": name, "name": SERVICE_NAMES[name], "enabled": name in self.enabled, "running": s.running(),
                    "error": s.error, "since": s.started_at if s.running() else 0,
                    "firewall": firewall_command(name, service_ports(self.cfg, name))}
            if name == "mp":
                item["ports"] = "TCP + UDP %d" % self.cfg["mp"]["port"]
                item["urls"] = ["Headsets and PCs enter: %s" % a for a in mp.addresses()] + ["(from the internet: your public IP address or DNS name%s)" % ("" if self.cfg["mp"]["port"] == 19800 else ":%d" % self.cfg["mp"]["port"])]
            elif name == "web":
                item["ports"] = "TCP %d (HTTP)%s, UDP %d (LAN games)" % (self.cfg["web"]["port"], ", TCP %d (HTTPS)" % self.cfg["web"]["https_port"] if self.cfg["web"]["https_port"] else "", lobby.DISCOVERY_PORT)
                item["urls"] = web.urls() if web.running() else []
            else:
                item["ports"] = "TCP %d (%s)" % (self.cfg["admin"]["port"], admin.scheme.upper())
                item["urls"] = admin.urls() if admin.running() else []
            services.append(item)

        games, players = [], []
        names = {}
        if st:
            for c in st.get("clients", []):
                names[c["id"]] = c["name"]
                players.append({"kind": "Internet (app)", "name": c["name"], "address": c["address"], "detail": "app " + c["app"],
                                "game": ("internet game %d" % c["game"]) if c["game"] else "",
                                "link": ("relay (%s)" % human_bytes(c["relay_bytes"])) if c["relay_bytes"] else "direct or not playing",
                                "since": c["since"], "udp": c["udp"]})
            for g in st.get("games", []):
                b = decode_beacon(g.get("beacon", ""))
                games.append({"where": "Internet", "id": g["id"], "private": g["private"],
                              "host": names.get(g["owner"], "(host changing)") if g["owner"] else "(host changing)",
                              "players": [names.get(m, "#%d" % m) for m in g["members"]],
                              "max": b.get("maxPlayers") if b else None,
                              "mode": b.get("modeName", "") if b else "", "started": bool(b and b.get("started")),
                              "listed": g["listed"], "since": g["since"], "version": b.get("version", "") if b else ""})
        if web.running():
            for g in serve.LOBBIES.games():
                games.append({"where": "Local network", "id": g["hostId"], "private": False, "host": g["hostName"],
                              "players": [], "count": g["players"], "max": g["maxPlayers"], "mode": g.get("modeName", ""),
                              "started": g["started"], "listed": True, "since": 0, "version": g["version"],
                              "address": g["address"]})
        browser_hosts = {}
        for cid, h in list(serve.LOBBIES.hosts.items()):
            if getattr(h, "hosting", False):
                browser_hosts[cid] = h
        now = time.time()
        for c in web.clients.snapshot():
            if now - c["last"] > 900 and not c["links"]:
                continue
            who = sorted({v["name"] for v in c["cids"].values() if v["name"]})
            role = []
            for cid in c["cids"]:
                if cid in browser_hosts:
                    h = browser_hosts[cid]
                    role.append("hosts a game (%s, %d players)" % ("match running" if h.started else "lobby",
                                                                   1 + sum(1 for c in list(getattr(h, "slots", []))[1:] if c)))
                else:
                    j = serve.LOBBIES.players.get(cid)
                    if j is not None and j.view.get("phase") not in (None, "idle"):
                        role.append("in a game at %s (%s)" % (j.view.get("hostAddress", "?"), j.view.get("phase")))
            players.append({"kind": "Web browser", "name": ", ".join(who), "address": c["address"], "detail": c["agent"],
                            "game": "; ".join(role) or ("playing online" if c["links"] else ""),
                            "link": "online" if c["links"] else ("active" if now - c["last"] < 60 else "idle"),
                            "since": c["first"], "last": c["last"], "udp": ""})

        settings = {
            "mp": dict(self.cfg["mp"], folder_resolved=mp.folder(), exe_found=os.path.isfile(mp.exe()),
                       key_file=os.path.join(mp.folder(), "tcvr-server.key"),
                       fingerprint=st.get("fingerprint", "") if st else "", version=st.get("version", "") if st else "",
                       protocol=st.get("protocol", "") if st else ""),
            "web": dict(self.cfg["web"], disc_resolved=web.disc(), disc_found=os.path.isfile(web.disc()),
                        lobby_udp=lobby.DISCOVERY_PORT, profiles=serve.PROFILES, www=serve.ROOT),
            "admin": dict(self.cfg["admin"], scheme=admin.scheme, password_file=PASSWORD_PATH),
            "app": {"config_file": CONFIG_PATH, "log_folder": LOG_DIR, "python": sys.version.split()[0],
                    "computer_addresses": serve.lan_addresses(), "started": self.started,
                    "services_at_start": ",".join(n for n in SERVICES if n in self.enabled)},
        }
        return {"now": now, "services": services, "games": games, "players": players, "settings": settings,
                "handshaking": st.get("handshaking", 0) if st else 0}

    # -- changes from the admin page

    def apply_settings(self, body, who):
        """body: {"mp": {...}, "web": {...}, "admin": {...}} with the changed values only."""
        new = json.loads(json.dumps(self.cfg))
        try:
            for section, values in body.items():
                if section not in DEFAULTS or not isinstance(values, dict):
                    return {"ok": False, "error": "unknown section %s" % section}
                for k, v in values.items():
                    if k not in DEFAULTS[section] or (section, k) == ("admin", "allow"):
                        return {"ok": False, "error": "%s.%s can't be changed here" % (section, k)}
                    new[section][k] = check_value(section, k, v)
        except ValueError as e:
            return {"ok": False, "error": str(e)}
        ports = {("mp", "port"): new["mp"]["port"], ("web", "port"): new["web"]["port"],
                 ("admin", "port"): new["admin"]["port"]}
        if new["web"]["https_port"]:
            ports[("web", "https_port")] = new["web"]["https_port"]
        seen = {}
        for k, p in ports.items():
            if p in seen:
                return {"ok": False, "error": "Port %d is set twice (%s %s and %s %s)" % ((p,) + seen[p] + k)}
            seen[p] = k
        changed = [s for s in SERVICES if new[s] != self.cfg[s]]
        if not changed:
            return {"ok": True, "changed": [], "message": "Nothing changed"}
        # a new port must be free before the service moves there
        for s in changed:
            for proto, port in service_ports(new, s):
                if (proto, port) not in service_ports(self.cfg, s) and not port_free(proto, port):
                    return {"ok": False, "error": "%s port %d is already used by another program" % (proto, port)}
        old = self.cfg
        self.cfg = new
        save_config(new)
        LOG.add("admin", "%s changed the settings of: %s" % (who, ", ".join(SERVICE_NAMES[s] for s in changed)))
        result = {"ok": True, "changed": changed, "firewall": {s: firewall_command(s, service_ports(new, s)) for s in changed}}
        for s in changed:
            if s == "admin":
                continue
            svc = self.services[s]
            if svc.running():
                svc.stop()
                if not svc.start():
                    # back to what worked
                    self.cfg = old
                    save_config(old)
                    svc.start()
                    return {"ok": False, "error": "%s didn't start with the new settings (%s): the old ones are back" % (SERVICE_NAMES[s], svc.error)}
        if "admin" in changed:
            scheme = self.services["admin"].scheme
            result["redirect"] = "%s://{host}:%d/" % (scheme, new["admin"]["port"])
            if old["admin"]["port"] != new["admin"]["port"]:
                def move():
                    time.sleep(0.8)
                    self.services["admin"].stop()
                    if not self.services["admin"].start():
                        self.cfg["admin"]["port"] = old["admin"]["port"]
                        save_config(self.cfg)
                        self.services["admin"].start()
                threading.Thread(target=move, daemon=True).start()
        return result

    def service_action(self, name, action, who):
        if name not in self.services or action not in ("start", "stop", "restart"):
            return {"ok": False, "error": "bad request"}
        if name == "admin":
            return {"ok": False, "error": "The admin page can't stop itself from here: close the server window"}
        svc = self.services[name]
        LOG.add("admin", "%s: %s %s" % (who, action, SERVICE_NAMES[name]))
        if action in ("stop", "restart"):
            svc.stop()
        ok = True
        if action in ("start", "restart"):
            self.enabled.add(name)
            ok = svc.start()
        return {"ok": ok, "error": svc.error}


def check_value(section, key, v):
    d = DEFAULTS[section][key]
    if key.endswith("port"):
        try:
            v = int(v)
        except (TypeError, ValueError):
            raise ValueError("%s: a port is a number" % key)
        if key == "https_port" and v == 0:
            return 0
        if not 1024 <= v <= 65535:
            raise ValueError("Ports go from 1024 to 65535 (%s %s)" % (section, key))
        if v == lobby.DISCOVERY_PORT:
            raise ValueError("Port %d is the LAN games port of the web server" % v)
        return v
    if key in ("max_clients", "relay_kbps"):
        try:
            v = int(v)
        except (TypeError, ValueError):
            raise ValueError("%s is a number" % key)
        if not 1 <= v <= 100000:
            raise ValueError("%s: 1 to 100000" % key)
        return v
    if key == "games":
        if v not in ("all", "public", "private"):
            raise ValueError("games: all, public or private")
        return v
    if key == "verbose":
        return bool(v)
    if key == "listen":
        v = str(v).strip()
        try:
            ipaddress.ip_address(v)
        except ValueError:
            raise ValueError("listen: an IP address of this computer, 0.0.0.0 for all, 127.0.0.1 for this computer only")
        return v
    if isinstance(d, str):
        return str(v).strip().strip('"')
    return v


def human_bytes(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024:
            return "%d %s" % (n, unit)
        n //= 1024
    return "%d TB" % n


def decode_beacon(hexed):
    try:
        kind, b = lobby.decode_datagram(bytes.fromhex(hexed))
    except (ValueError, IndexError):
        return None
    if kind != "beacon":
        return None
    b["modeName"] = lobby.MODE_NAMES[min(b["settings"].get("mode", 0), 2)]
    return b


def main():
    ap = argparse.ArgumentParser(description="Toy Commander server app (see the top of this file)")
    ap.add_argument("--services", nargs="+", default=["mp,web,admin"],
                    help="what to start, comma separated: mp (internet multiplayer), web (play in a browser), admin (admin page). Default: all")
    ap.add_argument("--disc", help="your disc image (default: ToyCommander.chd next to this file; saved in tcvr-host.json)")
    ap.add_argument("--mp-folder", help="the folder of tcvr-server.exe and its key (saved in tcvr-host.json)")
    ap.add_argument("--mp-port", type=int, help="internet multiplayer port (saved)")
    ap.add_argument("--web-port", type=int, help="web HTTP port (saved)")
    ap.add_argument("--https-port", type=int, help="web HTTPS port, 0 for none (saved)")
    ap.add_argument("--admin-port", type=int, help="admin page port (saved)")
    a = ap.parse_args()
    words = " ".join(a.services).replace(";", ",").replace(" ", ",")
    enabled = {s.strip().lower() for s in words.split(",") if s.strip()}
    if "all" in enabled:
        enabled = set(SERVICES)
    bad = enabled - set(SERVICES)
    if bad or not enabled:
        ap.error("--services takes mp, web and/or admin (got %s)" % words)
    cfg = load_config()
    try:
        for value, section, key in ((a.disc, "web", "disc"), (a.mp_folder, "mp", "folder"), (a.mp_port, "mp", "port"),
                                    (a.web_port, "web", "port"), (a.https_port, "web", "https_port"),
                                    (a.admin_port, "admin", "port")):
            if value is not None:
                cfg[section][key] = check_value(section, key, value)
    except ValueError as e:
        ap.error(str(e))
    save_config(cfg)
    host = Host(cfg, enabled)
    LOG.add("host", "Toy Commander server app: starting %s" % ", ".join(SERVICE_NAMES[s] for s in SERVICES if s in enabled))
    host.start()
    if "admin" in enabled and host.services["admin"].running():
        # on the screen only, not in the log file or the admin page's log
        print("Admin password: %s  (kept in %s%s)" % (host.password, PASSWORD_PATH, ", new" if host.new_password else ""), flush=True)
    LOG.add("host", "Close this window or press Ctrl+C to stop everything.")
    import signal
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        LOG.add("host", "Stopping")
        host.stop()


if __name__ == "__main__":
    main()
