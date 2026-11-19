/*
	tcvr: internet play, the wire formats and the cryptography shared by the game and the server app
	(tcvr/server). No emulator code here: the server is built from this file alone.

	Who talks to whom
	- Every player's copy keeps one TCP connection to the server ("control"). It is encrypted: a handshake
	  with the server's long-term X25519 key (pinned by the game the first time it meets a server name),
	  then XChaCha20-Poly1305 frames. On it: the games list, announcing a game, joining one, the members
	  of the game and the lobby's own connections (relayed byte streams, see Stream).
	- Every copy also has one UDP socket. It registers with the server (so the server learns its public
	  address), then sends the match's packets straight to the other players (NAT hole punching), or
	  through the server ("relay") when no direct path works.
	- What the players send each other (lobby streams, match packets) is encrypted end to end with a key
	  per pair of players: X25519 between their member keys (made fresh for each connection), mixed with
	  the game password when there is one. The server passes these bytes along and cannot read them.

	All numbers are little endian. Every length is checked before use: these bytes come from the network.

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tcvr::inet
{

constexpr uint16_t DefaultServerPort = 19800;	// TCP (control) and UDP (registration, relay), same number
constexpr uint8_t ProtocolVersion = 2;	// 2: private games (PIN)
constexpr size_t MaxFrame = 8192;				// control frame, plaintext
constexpr size_t MaxDatagram = 1400;			// any UDP datagram we send or accept
constexpr int MaxMembers = 8;					// a game's members on the server (4 players, and room for comings and goings)
constexpr size_t MaxNameLen = 16;
constexpr size_t MaxLanEndpoints = 4;

using Key = std::array<uint8_t, 32>;

// ---- primitives ------------------------------------------------------------------------------

// Bytes from the system's secure random source. Throws std::runtime_error if there is none.
void randomBytes(void *out, size_t size);
uint32_t randomU32();

struct KeyPair
{
	Key secret {};
	Key pub {};
	static KeyPair generate();
	static KeyPair fromSecret(const Key& secret);
	void wipe();
};

// A short text for a public key, to show and compare: "ab12-cd34-ef56-7890".
std::string fingerprint(const Key& pub);
std::string toHex(const uint8_t *data, size_t size);
bool fromHex(const std::string& hex, uint8_t *out, size_t size);

// ---- byte helpers ------------------------------------------------------------------------------

struct Writer
{
	std::vector<uint8_t> b;
	void u8(uint8_t v) { b.push_back(v); }
	void u16(uint16_t v) { u8((uint8_t)v); u8((uint8_t)(v >> 8)); }
	void u32(uint32_t v) { u16((uint16_t)v); u16((uint16_t)(v >> 16)); }
	void u64(uint64_t v) { u32((uint32_t)v); u32((uint32_t)(v >> 32)); }
	void bytes(const uint8_t *p, size_t n) { b.insert(b.end(), p, p + n); }
	void bytes(const std::vector<uint8_t>& v) { bytes(v.data(), v.size()); }
	void key(const Key& k) { bytes(k.data(), k.size()); }
	void str(const std::string& s);		// u8 length, then the bytes (cut at 255)
	void blob(const std::vector<uint8_t>& v);	// u16 length, then the bytes
};

struct Reader
{
	const uint8_t *p;
	size_t n;
	size_t at = 0;
	bool ok = true;
	Reader(const uint8_t *p, size_t n) : p(p), n(n) {}
	explicit Reader(const std::vector<uint8_t>& v) : p(v.data()), n(v.size()) {}
	bool need(size_t k) { if (!ok || n - at < k) ok = false; return ok; }
	uint8_t u8() { return need(1) ? p[at++] : 0; }
	uint16_t u16() { uint16_t lo = u8(); return (uint16_t)(lo | (u8() << 8)); }
	uint32_t u32() { uint32_t lo = u16(); return lo | ((uint32_t)u16() << 16); }
	uint64_t u64() { uint64_t lo = u32(); return lo | ((uint64_t)u32() << 32); }
	bool bytes(uint8_t *out, size_t k) { if (!need(k)) return false; std::copy(p + at, p + at + k, out); at += k; return true; }
	Key key() { Key k {}; bytes(k.data(), k.size()); return k; }
	std::string str(size_t maxLen = 255);
	std::vector<uint8_t> blob(size_t maxLen = MaxFrame);
	std::vector<uint8_t> rest() { std::vector<uint8_t> v(p + at, p + n); at = n; return v; }
	bool done() const { return ok && at == n; }
};

// ---- the encrypted control channel (TCP) -----------------------------------------------------
//
// client -> server  "TCVR" | version | client ephemeral public key (32)                  = 37 bytes
// server -> client  "TCVR" | version | server ephemeral public (32) | server static public (32) = 69 bytes
// keys = BLAKE2b-512("tcvr-inet-v1" | X25519(ce, se) | X25519(ce, ss) | ce.pub | se.pub | ss.pub)
// first 32 bytes: client -> server, last 32: server -> client. Only the holder of the server's static
// secret can derive them, so the first frame the client can open proves it talks to that server.
// Then frames: u16 length of what follows | ciphertext | 16-byte tag; the nonce is the frame counter.

constexpr size_t ClientHelloSize = 37;
constexpr size_t ServerReplySize = 69;

std::vector<uint8_t> clientHello(const KeyPair& ephemeral);
// Server side. False: not our protocol.
bool serverHandshake(const uint8_t *hello, size_t size, const KeyPair& serverStatic, std::vector<uint8_t>& reply,
		uint8_t keys[64]);
// Client side. False: not a tcvr server.
bool clientHandshake(const KeyPair& ephemeral, const uint8_t *reply, size_t size, Key& serverStatic, uint8_t keys[64]);

class Channel
{
public:
	// keys: from the handshake; server: which side this is
	void init(const uint8_t keys[64], bool server);
	std::vector<uint8_t> seal(const std::vector<uint8_t>& plain);
	// Cuts the stream into frames and opens them. False: a frame failed (forged, damaged, too long):
	// drop the connection.
	bool feed(const uint8_t *data, size_t size, std::vector<std::vector<uint8_t>>& out);
	void wipe();
private:
	Key sendKey {}, recvKey {};
	uint64_t sendCount = 0, recvCount = 0;
	std::vector<uint8_t> buffer;
};

// ---- control messages (inside the channel): u8 type, then the fields ------------------------

enum class Ctl : uint8_t
{
	Hello = 1,		// c->s  protocol, app version, name, member key, LAN endpoints of its UDP socket
	Welcome,		// s->c  client id, UDP key, server UDP port, the address the server sees
	List,			// c->s  the games, please
	Games,			// s->c  [owner id, flags, the lobby's beacon bytes]...
	Announce,		// c->s  flags, the lobby's beacon bytes: I host this game; creates the game or updates it
	Unannounce,		// c->s  take my game off the list (it keeps its members)
	Connect,		// c->s  target id: put me in the game the target is in
	Connected,		// s->c  target id, ok (u8), reason text
	LeaveGame,		// c->s
	Members,		// s->c  the members of my game (MemberInfo...)
	Stream,			// both  peer id (to on c->s, from on s->c), stream id, kind, data
	Error,			// s->c  text; the server closes after it
	Ping,			// both
	Pong,
};

struct Endpoint
{
	uint32_t ip = 0;		// IPv4, host order (a.b.c.d = a<<24 | ...); 0 = none
	uint16_t port = 0;
	bool valid() const { return ip != 0 && port != 0; }
	bool operator==(const Endpoint& o) const { return ip == o.ip && port == o.port; }
	bool operator!=(const Endpoint& o) const { return !(*this == o); }
	bool operator<(const Endpoint& o) const { return ip != o.ip ? ip < o.ip : port < o.port; }
	std::string text() const;		// "a.b.c.d:port"
	std::string address() const;	// "a.b.c.d"
	static bool parse(const std::string& address, uint16_t port, Endpoint& out);
};
void put(Writer& w, const Endpoint& e);
Endpoint getEndpoint(Reader& r);

struct HelloMsg
{
	uint8_t protocol = ProtocolVersion;
	std::string appVersion;
	std::string name;
	Key memberKey {};
	std::vector<Endpoint> lanAddrs;
};
// Private games: joining needs the game's PIN (4 digits). The PIN only admits players: the host's copy
// checks it (with a growing wait after wrong ones), the server never sees it, and the encryption keys
// stay the players' own (a 4-digit number would be far too easy to guess as a key).
constexpr size_t PinLength = 4;
bool validPin(const std::string& pin);		// "" (a public game) or exactly 4 digits
enum class GamePolicy : uint8_t { All = 0, PublicOnly = 1, PrivateOnly = 2 };
const char *policyText(GamePolicy p);		// "public and private games", ...
bool policyAllows(GamePolicy p, bool isPrivate);

struct WelcomeMsg
{
	uint32_t clientId = 0;
	Key udpKey {};
	uint16_t udpPort = 0;
	Endpoint seenAs;		// the TCP source address as the server saw it (port: TCP's)
	GamePolicy policy = GamePolicy::All;	// which games this server lets players host
};
struct GameEntry
{
	uint32_t ownerId = 0;
	bool isPrivate = false;		// a PIN is needed to join (the host's copy checks it, never the server)
	std::vector<uint8_t> beacon;
};
struct MemberInfo
{
	uint32_t clientId = 0;
	std::string name;
	Key memberKey {};
	Endpoint udp;					// as the server saw its UDP registration; invalid until then
	std::vector<Endpoint> lanAddrs;
	bool owner = false;
};
// Open: sealed u16 port + PIN text. Refused: sealed u8 RefuseWhy (then the stream is gone).
enum class StreamKind : uint8_t { Open = 1, Data = 2, Close = 3, Refused = 4 };
enum class RefuseWhy : uint8_t { WrongPin = 1, TooManyTries = 2 };
struct StreamMsg
{
	uint32_t peer = 0;
	uint32_t streamId = 0;		// chosen by the opener; the pair (opener, id) names the stream
	StreamKind kind = StreamKind::Data;
	std::vector<uint8_t> data;	// Open: u16 port (sealed); Data: sealed bytes; Close: nothing
};

std::vector<uint8_t> encode(const HelloMsg& m);
bool decode(Reader& r, HelloMsg& m);
std::vector<uint8_t> encode(const WelcomeMsg& m);
bool decode(Reader& r, WelcomeMsg& m);
std::vector<uint8_t> encodeGames(const std::vector<GameEntry>& games);
bool decodeGames(Reader& r, std::vector<GameEntry>& games);
std::vector<uint8_t> encodeAnnounce(const std::vector<uint8_t>& beacon, bool isPrivate);
std::vector<uint8_t> encodeConnect(uint32_t target);
std::vector<uint8_t> encodeConnected(uint32_t target, bool ok, const std::string& why);
std::vector<uint8_t> encodeMembers(const std::vector<MemberInfo>& members);
bool decodeMembers(Reader& r, std::vector<MemberInfo>& members);
std::vector<uint8_t> encode(const StreamMsg& m);
bool decode(Reader& r, StreamMsg& m);
std::vector<uint8_t> encodeError(const std::string& text);
std::vector<uint8_t> encodeSimple(Ctl type);

// ---- UDP -------------------------------------------------------------------------------------
//
// With the server: u8 type | u32 client id | payload | 16-byte BLAKE2b MAC keyed with that client's UDP
// key (given in Welcome over the encrypted channel), over everything before it.
//   Register   c->s  u32 sequence                       the server notes the source address
//   Registered s->c  u32 sequence | endpoint seen
//   Relay      c->s  u32 to | peer packet               the server sends it on to "to" as Relayed
//   Relayed    s->c  u32 from | peer packet             (client id field: the receiver)
// Between players: u8 Peer | u32 from | u64 counter | ciphertext | 16-byte tag (AD = the 13-byte header),
// with the pair key; the same packet goes direct or inside Relay/Relayed.

enum class UdpType : uint8_t { Register = 1, Registered = 2, Relay = 3, Relayed = 4, Peer = 0x10 };
constexpr size_t UdpMacSize = 16;
constexpr size_t PeerHeaderSize = 13;
constexpr size_t PeerOverhead = PeerHeaderSize + 16;

std::vector<uint8_t> sealServerUdp(UdpType type, uint32_t clientId, const std::vector<uint8_t>& payload, const Key& udpKey);
// Reads type and client id (to find the key), then checks the MAC with keyFor(clientId).
// False: too short, unknown client or a bad MAC.
template <typename KeyFor>
bool openServerUdp(const uint8_t *data, size_t size, UdpType& type, uint32_t& clientId, std::vector<uint8_t>& payload, KeyFor keyFor);
bool checkServerUdpMac(const uint8_t *data, size_t size, const Key& udpKey);

// What one player shares with another: the key, and which of the two it is (nonces never repeat
// across the two directions or between the UDP packets and the lobby streams).
struct PairKey
{
	Key key {};
	uint8_t dir = 0;			// 0 or 1: this side's direction
	bool valid = false;
	void wipe();
};
// password: "" for none; both sides must use the same one or nothing opens.
PairKey derivePairKey(const KeyPair& mine, const Key& theirs, const std::string& password);

// domain: 0 UDP packets, 1 lobby streams
std::vector<uint8_t> sealPeer(const PairKey& k, uint8_t domain, uint32_t fromId, uint64_t counter, const std::vector<uint8_t>& plain);
// UDP peer packet: reads from and counter, then opens with k (the caller finds k by from first: peekPeerFrom)
bool peekPeerFrom(const uint8_t *data, size_t size, uint32_t& fromId);
bool openPeer(const PairKey& k, uint8_t domain, const uint8_t *data, size_t size, uint64_t& counter, std::vector<uint8_t>& plain);

// Anti-replay for UDP: a counter is accepted once, and only within the last 1024.
class ReplayWindow
{
public:
	bool accept(uint64_t counter);
private:
	uint64_t top = 0;
	std::array<uint64_t, 16> bits {};
};

// ---- the lobby's addresses ---------------------------------------------------------------------
// The lobby (tcvr_lobby) names players by IPv4 address. In internet play a player's "address" is its
// client id on the server, written as 10.x.y.z (ids are below 2^24). Nothing is ever sent to it: the
// router turns it into local relay ports.
std::string virtualAddress(uint32_t clientId);
uint32_t clientIdOf(const std::string& virtualAddress);		// 0: not one

// ---- template body ---------------------------------------------------------------------------

template <typename KeyFor>
bool openServerUdp(const uint8_t *data, size_t size, UdpType& type, uint32_t& clientId, std::vector<uint8_t>& payload, KeyFor keyFor)
{
	if (size < 5 + UdpMacSize || size > MaxDatagram)
		return false;
	type = (UdpType)data[0];
	clientId = (uint32_t)data[1] | ((uint32_t)data[2] << 8) | ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
	const Key *k = keyFor(clientId);
	if (k == nullptr || !checkServerUdpMac(data, size, *k))
		return false;
	payload.assign(data + 5, data + size - UdpMacSize);
	return true;
}

}
