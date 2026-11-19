# Internet play

Multiplayer over the internet uses the same lobby and match as on the local network; only the way the
headsets reach each other changes. A small server program from this repository (`tcvr/server`) is the
meeting point. Whoever wants to host a server runs it (Windows or Debian, see `tcvr/server/README.md`).

## In the headset

VR menu > Multiplayer:

- **Play over**: Local network (Wi-Fi) or Internet.
- **Server** (Internet): the server's address, an IP address or a DNS name, with `:port` if not 19800.
  The line under it says whether the headset is online, and the server key's fingerprint.
- **Create game / Join game** work as on the local network. The game list comes from the server.
- In the lobby, each other player has a line "Link to BBB direct 45 ms" (or "via the server").

The player who creates the game is the host. If the host leaves before the match, the next player in
slot order takes over the same game (the LAN host handover, protocol 2); during a match, a leaving player
is handled as on the LAN.

## How it works

| What | Path | Protection |
| --- | --- | --- |
| Headset to server (games list, joining, members) | TCP to the server | X25519 handshake with the server's key (pinned per address), XChaCha20-Poly1305 frames |
| Lobby messages between players | through the server, as relayed streams | end to end: X25519 between the two players' member keys, XChaCha20-Poly1305, the server can't read them |
| Match packets (GGPO) | direct UDP between headsets after hole punching; else relayed by the server | end to end, as above, with anti-replay |

- The game's lobby code is unchanged on the wire: in internet mode its connections go to local ports of
  the internet client (`core/network/tcvr_inet.*`), which carries them. Players are named by their number
  on the server, written as an address `10.x.y.z` that is never routed.
- For the match, GGPO sends to `127.0.0.1` ports, one per other player; the client sends each packet
  straight to that player if the two have a direct path, else through the server.
- Direct paths: each headset registers its UDP socket with the server (which learns its public address),
  then the players send each other small "punch" packets on every address they know (public, LAN, and
  addresses learned from arriving packets). A path is used once the other side says it hears it; it
  falls back to the server after 3 s of silence and is tried again.
- Game password (option `password` in `ClientConfig`, not in the menu yet): mixed into the player keys, so
  even a server that lies about the players' keys can't read or join. Without it, the server could in
  principle put itself in the middle of a new game by handing out false keys; it can never read a game
  between players whose keys it passed honestly.

## Limits of this first version

- IPv4 only (server address and paths).
- The relay costs the server about 20 KB/s per player in a 4-player match.
- Not yet tested with the emulator on headsets; cloud tests use the real lobby and network code with a
  fake match (see the server README, Tests).
