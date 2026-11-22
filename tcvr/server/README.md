# tcvr-server

The internet meeting point for Toy Commander VR multiplayer. It does **not** run the game:

- it lists the games that players host,
- it introduces the players of a game to each other, so their headsets can reach each other directly
  through their home routers (UDP hole punching),
- it passes the lobby's messages along, and the match's packets when no direct path works ("relay").

What players send each other (lobby and match) is encrypted end to end with keys made by the headsets;
the server only forwards bytes it cannot read. The link between a headset and the server is encrypted too.

One port, TCP and UDP, default **19800**.

## Windows (PowerShell)

1. Put `tcvr-server.exe` and `run-server.ps1` in one folder.
2. Once, as administrator, to allow it in Windows Firewall: `.\run-server.ps1 -OpenFirewall`
3. Then just `.\run-server.ps1` (Ctrl+C stops it). `-Port 20000` for another port, `-VerboseLog` for more lines.

PowerShell may refuse to run a downloaded script: `powershell -ExecutionPolicy Bypass -File .\run-server.ps1`.

## Debian / Ubuntu server

From a copy of this repository:

    sudo tcvr/server/debian/install.sh          # or: install.sh 20000 for another port

It builds the server, installs it as the `tcvr-server` systemd service (user `tcvr`, key in
`/var/lib/tcvr-server`), starts it and prints the key fingerprint. Logs: `journalctl -u tcvr-server -f`.
Change the port or add options in `/etc/default/tcvr-server`, then `systemctl restart tcvr-server`.
With ufw: `ufw allow 19800/tcp && ufw allow 19800/udp`.

## Build by hand

    cmake -S tcvr/server -B build-server -DCMAKE_BUILD_TYPE=Release
    cmake --build build-server --config Release

Linux needs g++ and cmake; Windows needs Visual Studio (C++) or MinGW. It uses only this repository
(`core/network/tcvr_inet_proto.*`, Monocypher, asio). `tcvr-inet-peer` is a test player (see below).

## Public and private games

A player who creates a game chooses Public or Private. A private game is listed with a lock and needs its
4-digit PIN to join. The PIN is checked by the host's headset, never by the server; the server only limits
how often one address may try to join private games (10 a minute), so PINs can't be guessed quickly.
`--games public` or `--games private` makes the server accept only that kind; the log says which kind
each new game is.

## The router

Players connect to the server's **public** address (an IP address or a DNS name, a dynamic DNS name
works). If the server is at home behind a router, forward TCP **and** UDP 19800 to the server computer.

## The key

On its first run the server makes `tcvr-server.key`. A headset shows the key's fingerprint
(`Multiplayer > Server`, e.g. `4fc8-4f42-8e6b-cf6f`) and remembers the key for that server address. If the
key ever changes (a new file, or someone in the middle), the headset stops with "The server's key
changed". Entering the server address again in the VR menu trusts the new key.

Keep the key file private and back it up with the server. On Windows, keep it in a folder only the
server's account can read.

## Options

    --port N          TCP and UDP port (default 19800)
    --key FILE        the key file (default tcvr-server.key in the current folder)
    --max-clients N   at most this many connected headsets (default 200)
    --relay-kbps N    relay budget per headset, KB/s (default 256; a 4-player match needs about 20)
    --games KIND      which games players may host: all (default), public or private
    --verbose         more log lines
    --fingerprint     print the key's fingerprint and exit

## Tests

`tcvr/tools/inet-nat-test.sh` (Linux, root) builds a fake internet with network namespaces: the server,
players behind home routers (NAT), a "symmetric" router that forces the relay, two players behind the
same router, 80 ms and 2% loss, the host of a private game leaving the lobby, a wrong PIN. `tcvr-inet-peer` is
the test player it runs (the real lobby and internet code, a fake match instead of the emulator).
