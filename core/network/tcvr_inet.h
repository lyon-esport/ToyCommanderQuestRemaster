/*
	tcvr: internet play, the game's side (the server app is tcvr/server). See tcvr_inet_proto.h for what
	goes on the wire.

	InternetClient is the lobby's router (tcvr_lobby.h) for internet play:
	- it keeps the encrypted connection to the server the player entered (IP address or DNS name), and
	  pins the server's key the first time (a changed key stops it: "the server's key changed");
	- the games list comes from the server; hosting announces the game there;
	- each lobby connection to another player becomes a relayed stream through the server, encrypted end
	  to end with that player;
	- for the match, every other player gets a local UDP port that GGPO sends to (network:GGPOPeers);
	  the packets go straight to that player once the two found a way through their routers (hole
	  punching), else through the server; both ways they are encrypted end to end.

	No emulator code here: the tests (tests/src/TcvrInetTest.cpp, tcvr/tools/inet-nat-test.sh) run it
	with the server and real lobbies.

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#pragma once
#include "tcvr_inet_proto.h"
#include "tcvr_lobby.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tcvr::inet
{

struct ClientConfig
{
	std::string server;			// "host", "host:port", "a.b.c.d", "a.b.c.d:port"
	std::string name;			// the player's name
	std::string appVersion;
	std::string pinnedKey;		// hex of the server's key as first met ("" = not met yet: trust and report it)
	std::string password;		// the game password ("" none); every player of a game must use the same
	uint16_t udpPort = 0;		// this copy's UDP port (0: any)
	bool forceRelay = false;	// tests: never use a direct path
	bool noLan = false;			// tests: don't offer this computer's LAN addresses
};

enum class Status
{
	Off,			// not configured
	Resolving,
	Connecting,
	Online,
	Failed,			// will try again in a few seconds
	KeyChanged,		// stopped: the server's key is not the one pinned
	Refused,		// stopped: the server refused this copy (another version)
};

struct PeerPath
{
	uint32_t clientId = 0;
	std::string name;
	bool direct = false;		// the match's packets go straight to this player
	int rttMs = -1;			// last measured round trip (-1: none yet)
};

class InternetClient : public lobby::Router
{
public:
	InternetClient();
	~InternetClient() override;
	InternetClient(const InternetClient&) = delete;
	InternetClient& operator=(const InternetClient&) = delete;

	// Connect (or reconnect if anything changed). Thread-safe.
	void configure(const ClientConfig& config);
	void stop();

	// For the menu: the state and a line about it.
	Status status(std::string *line = nullptr);
	// The server's key as met by this connection (hex), and its fingerprint (for the menu and the pin).
	std::string serverKey();
	std::string serverFingerprint();
	// Called (on the client's thread) when a server is met for the first time: save the pin.
	void onNewServerKey(std::function<void(const std::string& server, const std::string& keyHex)> f);

	// The match: network:GGPOPeers for this copy, every other player behind a local UDP port.
	// hostAddress fills the host's entry (StartInfo::peerList). Empty: a player is not known (any more).
	std::string ggpoPeers(const lobby::StartInfo& info, const std::string& hostAddress, uint16_t myGgpoPort);
	// The match is over: the local UDP ports close.
	void matchOver();
	std::vector<PeerPath> paths();

	// lobby::Router
	bool ready(std::string& why) override;
	std::string localAddress() override;
	uint16_t route(const std::string& address, uint16_t port) override;
	std::string peerOf(uint16_t localPort) override;
	void listening(uint16_t port, bool on) override;
	void announce(const lobby::Beacon *beacon) override;
	void search(bool on) override;
	std::vector<lobby::FoundGame> games() override;
	void idle() override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

// "host:port" -> host and port (DefaultServerPort when none). False: empty or a bad port.
bool splitServer(const std::string& text, std::string& host, uint16_t& port);

}
