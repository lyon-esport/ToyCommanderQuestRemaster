/*
	tcvr: LAN lobby for the VR multiplayer (Toy Commander VR, multiplayer tasks 3.1 and 3.2).

	No external server. A host announces its game with one UDP broadcast per second (the "beacon");
	a joiner listens while its Join page is open and lists the games seen in the last 3 seconds, or
	asks one address directly ("Enter address"). Then the joiner opens a TCP connection to the lobby
	port named in the beacon and the two talk in small framed messages:

		HELLO   joiner -> host   name, app version, protocol, disc hash, GGPO port
		WELCOME host -> joiner   slot (0-3)
		REFUSE  host -> joiner   reason: full, version, disc, started
		STATE   host -> all      the whole lobby (settings, players with slot, name, ready, address:port)
		READY   joiner -> host   ready or not
		LEAVE   either way       leaving cleanly
		PING    either way       keep-alive every 2 s; a side silent for 6 s is dropped
		START   host -> all      the players in slot order as address:port, this copy's index, input delay

	On START the app sets network:GGPOPeers / network:GGPOPlayer, turns netplay on and restarts the
	game (tcvr_lobby_glue). Everything here is plain asio and the standard library, so the protocol
	has unit tests that run without the emulator (tests/src/TcvrLobbyTest.cpp).

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tcvr::lobby
{

constexpr int MaxPlayers = 4;
constexpr uint16_t DiscoveryPort = 19790;	// UDP; not the GGPO port (19713)
constexpr uint8_t ProtocolVersion = 1;
constexpr size_t NameMax = 8;				// the menu offers 3 letters; a longer name from elsewhere is cut

// The game's multiplayer options page, as the game shows it (measured on PC, 2026-11-18).
constexpr int RoomCount = 8;
extern const char *const RoomNames[RoomCount];
extern const char *const ModeNames[3];
// The limit line ("Number of Points to Win"): one index 0..21 for every mode; 0 is no limit ("---"), then
// points in Deathmatch and Capture the Flag, seconds in Cat and Mice. The game's default is index 10.
constexpr int LimitCount = 22;
constexpr int DefaultLimitIndex = 10;
int limitValue(int mode, int index);		// 0 for no limit
int limitIndex(int mode, int value);		// the nearest step
std::string limitText(int mode, int value);	// "15 points to win", "8 min time limit", "no limit"

// What the host chose for the match; the automatic setup (tcvr_match.cpp) enters it in the game.
struct Settings
{
	uint8_t mode = 0;		// 0 Deathmatch, 1 Cat and Mice, 2 Capture the Flag
	uint8_t room = 0;		// 0..7, the eight arenas (RoomNames)
	uint16_t points = 10;	// the limit: points to win, seconds in Cat and Mice, 0 none (limitValue)
	bool operator==(const Settings& o) const { return mode == o.mode && room == o.room && points == o.points; }
	bool operator!=(const Settings& o) const { return !(*this == o); }
};

// Who this copy is.
struct Identity
{
	std::string name;				// the player's name (3 letters)
	std::string appVersion;			// copies with another version are refused
	std::string gameId;				// the disc's game ID
	std::array<uint8_t, 16> disc {};// the disc's hash: copies of another disc are refused
	uint16_t ggpoPort = 19713;		// this copy's netplay port (UDP), given to the other players
};

enum class RefuseReason : uint8_t { None = 0, Full = 1, Version = 2, Disc = 3, Started = 4 };

// ---- messages on the wire -------------------------------------------------------------------

enum class MsgType : uint8_t { Hello = 1, Welcome = 2, Refuse = 3, State = 4, Ready = 5, Leave = 6, Ping = 7, Start = 8 };

struct Beacon
{
	uint32_t hostId = 0;			// random, tells copies of the same host apart from two hosts
	uint8_t protocol = ProtocolVersion;
	std::string appVersion;
	std::string gameId;
	std::string hostName;
	Settings settings;
	uint8_t players = 1;			// joined, host included
	uint8_t maxPlayers = MaxPlayers;
	uint16_t lobbyPort = 0;			// the TCP port to join
	bool started = false;
};

struct Hello
{
	uint8_t protocol = ProtocolVersion;
	std::string name;
	std::string appVersion;
	std::array<uint8_t, 16> disc {};
	uint16_t ggpoPort = 0;
};

struct PlayerInfo
{
	bool present = false;
	std::string name;
	bool ready = false;
	std::string address;			// as the host sees it ("" for the host itself)
	uint16_t ggpoPort = 0;
};

struct State
{
	uint32_t revision = 0;
	Settings settings;
	std::array<PlayerInfo, MaxPlayers> players;
	int count() const;
};

struct Peer
{
	std::string address;			// "" in the host's entry: the address this copy joined by
	uint16_t port = 0;
};

struct StartInfo
{
	std::vector<Peer> peers;		// in player order
	int localIndex = 0;				// this copy's place in peers
	uint8_t inputDelay = 2;
	uint32_t sessionId = 0;
	Settings settings;
	// the list for network:GGPOPeers ("address:port,address:port"); hostAddress fills the host's entry
	std::string peerList(const std::string& hostAddress) const;
};

// Frames: u16 length (of what follows), u8 type, payload. Little endian throughout.
std::vector<uint8_t> encodeFrame(MsgType type, const std::vector<uint8_t>& payload = {});
// Cuts the byte stream into frames as it arrives.
class FrameParser
{
public:
	struct Frame { MsgType type; std::vector<uint8_t> payload; };
	// False: the stream is broken (a frame too long or of unknown type): drop the connection.
	bool feed(const uint8_t *data, size_t size, std::vector<Frame>& out);
private:
	std::vector<uint8_t> buffer;
};

std::vector<uint8_t> encode(const Hello& m);
bool decode(const std::vector<uint8_t>& p, Hello& m);
std::vector<uint8_t> encodeWelcome(uint8_t slot);
bool decodeWelcome(const std::vector<uint8_t>& p, uint8_t& slot);
std::vector<uint8_t> encodeRefuse(RefuseReason reason);
bool decodeRefuse(const std::vector<uint8_t>& p, RefuseReason& reason);
std::vector<uint8_t> encode(const State& m);
bool decode(const std::vector<uint8_t>& p, State& m);
std::vector<uint8_t> encodeReady(bool ready);
bool decodeReady(const std::vector<uint8_t>& p, bool& ready);
std::vector<uint8_t> encode(const StartInfo& m);
bool decode(const std::vector<uint8_t>& p, StartInfo& m);

// UDP datagrams of the discovery port.
std::vector<uint8_t> encode(const Beacon& m);
std::vector<uint8_t> encodeQuery();
enum class Datagram { Invalid, Beacon, Query };
Datagram decodeDatagram(const uint8_t *data, size_t size, Beacon& beacon);

// A name made of printable ASCII, 1 to NameMax characters ("???" when nothing is left).
std::string cleanName(const std::string& name);

// ---- the lobby ------------------------------------------------------------------------------

struct Timing
{
	int beaconMs = 1000;		// host: one beacon per second
	int visibleMs = 3000;		// joiner: a game not heard for this long leaves the list
	int pingMs = 2000;			// keep-alive
	int dropMs = 6000;			// silent for this long: gone
	int connectMs = 5000;		// joiner: no answer from the host
};

struct FoundGame
{
	uint32_t hostId = 0;
	std::string address;		// where its beacon came from
	Beacon beacon;
};

enum class Role { None, Host, Joiner };
enum class Phase
{
	Idle,
	Connecting,		// joiner: waiting for the host's WELCOME
	InLobby,
	Started,		// START arrived (or sent): start() holds the session
	Refused,		// joiner: see refuse
	HostLeft,		// joiner: the host went away
	Failed			// something else; see message
};

struct View
{
	Role role = Role::None;
	Phase phase = Phase::Idle;
	RefuseReason refuse = RefuseReason::None;
	std::string message;		// a line for the menu when the phase is not InLobby
	State state;
	int mySlot = -1;
	std::string hostAddress;	// the address to give another player to join by hand
	uint16_t lobbyPort = 0;
	StartInfo start;			// phase Started
};

// The line the menu shows for a refusal.
const char *refuseText(RefuseReason reason);

// ---- the address a host announces ----------------------------------------------------------
// A computer can hold several IPv4 addresses: the real LAN (Wi-Fi, Ethernet, or a Hyper-V
// external switch bridged to it), host-only virtual networks (Hyper-V "Default Switch", WSL,
// VirtualBox, VMware, Docker), VPN tunnels, loopback and link-local. The host must announce the
// one the other headsets can reach. The adapter that holds the default gateway wins; then a
// physical Wi-Fi/Ethernet adapter; virtual and VPN adapters only when nothing else is there.

struct NetInterface
{
	std::string name;			// "Wi-Fi", "wlan0", "vEthernet (Default Switch)"
	std::string description;	// Windows adapter description ("Hyper-V Virtual Ethernet Adapter"); may be empty
	std::string address;		// dotted IPv4
	bool up = true;
	bool loopback = false;
	bool gateway = false;		// holds an IPv4 default gateway (Windows), or is the default route's interface
	bool wireless = false;		// known to be Wi-Fi from the OS adapter type
	bool wired = false;			// known to be Ethernet from the OS adapter type
	bool tunnel = false;		// known to be a tunnel/PPP adapter from the OS adapter type
	uint32_t metric = 0;		// lower is preferred (Windows interface metric; 0 = unknown)
};

// Score of one interface for announcing; negative = never usable (down, loopback, link-local, no address).
int lanScore(const NetInterface& nif);
// The best address in the list, or "" when none is usable.
std::string pickLanAddress(const std::vector<NetInterface>& interfaces);
// This computer's interfaces (Windows: GetAdaptersAddresses; elsewhere getifaddrs plus the default route).
std::vector<NetInterface> listInterfaces();

class Lobby
{
public:
	Lobby(const Timing& timing = Timing());
	~Lobby();
	Lobby(const Lobby&) = delete;
	Lobby& operator=(const Lobby&) = delete;

	// What a platform needs for receiving broadcasts (Android: the multicast lock). Called with true
	// when listening starts, false when it ends.
	void setBroadcastHook(std::function<void(bool)> hook);
	// More places to send the beacon to besides the broadcast address and this computer (tests).
	void addBeaconTarget(const std::string& address);
	// Where beacons and queries go on this computer's port (tests: several copies, one port).
	void setDiscoveryPort(uint16_t port);

	// -- Join page
	void startSearch();
	void stopSearch();
	std::vector<FoundGame> games();			// seen within the last 3 s, oldest first
	void query(const std::string& address);	// "Enter address": ask this one address

	// -- Create game
	// False and a text in error: nothing could be opened.
	bool host(const Identity& me, const Settings& settings, std::string& error);
	void setSettings(const Settings& settings);		// host only
	// Host: START to everybody. False and a reason in error: not enough players or not all ready.
	bool start(uint8_t inputDelay, std::string& error);
	// The host can start: at least two players and every joiner ready.
	bool canStart(std::string& why);

	// -- Join game
	void join(const Identity& me, const std::string& address, uint16_t lobbyPort);
	void setReady(bool ready);

	// -- both
	// The match that START began is over (ended normally or a player left): back to the lobby page with
	// the same players, nobody ready, a host announces its game again. A joiner whose host went away
	// during the match ends in HostLeft. Nothing happens outside phase Started.
	void matchOver();
	void leave();							// tells the others and goes back to Idle
	View view();							// a copy of what the menu needs

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

}
