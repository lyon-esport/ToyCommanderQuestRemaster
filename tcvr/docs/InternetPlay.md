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
- **Game: Public / Private** (Create game, internet only). A private game needs a **4-digit PIN** to join
  (a random one is offered, it can be changed); the lobby shows it to the host to pass on. Private games
  are marked `[private]` in the Join list and ask for the PIN; a wrong one says "Wrong PIN".
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
- Private games: the PIN only admits players; it is not an encryption key (4 digits are only 10,000
  values, far too easy to try them all against a key). The host's copy checks it (the server never sees
  it), and wrong tries are slowed down twice: the host's copy makes the same player wait 2 s, 4 s, ... up to
  60 s after each wrong PIN, and the server lets one address try to join private games at most 10 times a
  minute, so trying every PIN from one address takes over 16 hours.
- The encryption keys are the players' own (X25519). A server that lied about the players' keys could in
  principle put itself in the middle of a new game; it can never read a game between players whose keys it
  passed honestly. Run your own server, or one you trust.
- The server can be told which games it accepts: `--games all|public|private` (the headset says so when a
  player tries another kind).

## Limits of this first version

- IPv4 only (server address and paths).
- The relay costs the server about 20 KB/s per player in a 4-player match.
- Not yet tested with the emulator on headsets; cloud tests use the real lobby and network code with a
  fake match (see the server README, Tests).
