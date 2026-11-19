/*
	tcvr: tests of the multiplayer lobby (core/network/tcvr_lobby.*): the message encoding, then real
	lobbies talking over this computer's loopback (host, 1 to 3 joiners, refusals, leaving, keep-alive).
*/
#include "gtest/gtest.h"
#include "network/tcvr_lobby.h"

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#include <asio.hpp>

#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>

using namespace tcvr::lobby;

namespace
{

State sampleState()
{
	State s;
	s.revision = 7;
	s.settings = { 2, 5, 90 };
	s.players[0] = { true, "MAN", true, "", 19713 };
	s.players[2] = { true, "ZOE", false, "192.0.2.7", 19714 };
	return s;
}

bool waitFor(const std::function<bool()>& cond, int ms = 3000)
{
	const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (std::chrono::steady_clock::now() < end)
	{
		if (cond())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return cond();
}

Identity ident(const std::string& name, uint16_t ggpo, const std::string& version = "1.0", uint8_t disc = 1)
{
	Identity id;
	id.name = name;
	id.appVersion = version;
	id.gameId = "MK-5102050";
	id.disc.fill(disc);
	id.ggpoPort = ggpo;
	return id;
}

// each test uses its own discovery port, so tests can't hear each other
uint16_t nextPort()
{
	static uint16_t p = 29700;
	return p++;
}

}

TEST(TcvrLobbyCodec, HelloRoundTrip)
{
	Hello h;
	h.name = "ABC";
	h.appVersion = "2026.11.18";
	h.disc.fill(0xAB);
	h.ggpoPort = 19715;
	Hello out;
	ASSERT_TRUE(decode(encode(h), out));
	EXPECT_EQ(out.name, "ABC");
	EXPECT_EQ(out.appVersion, "2026.11.18");
	EXPECT_EQ(out.disc, h.disc);
	EXPECT_EQ(out.ggpoPort, 19715);
	EXPECT_EQ(out.protocol, ProtocolVersion);
}

TEST(TcvrLobbyCodec, StateRoundTrip)
{
	const State s = sampleState();
	State out;
	ASSERT_TRUE(decode(encode(s), out));
	EXPECT_EQ(out.revision, 7u);
	EXPECT_EQ(out.settings, s.settings);
	EXPECT_EQ(out.count(), 2);
	EXPECT_TRUE(out.players[0].present);
	EXPECT_FALSE(out.players[1].present);
	EXPECT_EQ(out.players[2].name, "ZOE");
	EXPECT_EQ(out.players[2].address, "192.0.2.7");
	EXPECT_EQ(out.players[2].ggpoPort, 19714);
	EXPECT_TRUE(out.players[0].ready);
	EXPECT_FALSE(out.players[2].ready);
}

TEST(TcvrLobbyCodec, StartRoundTripAndPeerList)
{
	StartInfo s;
	s.peers = { { "", 19713 }, { "192.0.2.7", 19714 }, { "192.0.2.8", 19715 } };
	s.localIndex = 2;
	s.inputDelay = 3;
	s.sessionId = 0xCAFEBABE;
	s.settings = { 1, 3, 30 };
	StartInfo out;
	ASSERT_TRUE(decode(encode(s), out));
	EXPECT_EQ(out.peers.size(), 3u);
	EXPECT_EQ(out.localIndex, 2);
	EXPECT_EQ(out.inputDelay, 3);
	EXPECT_EQ(out.sessionId, 0xCAFEBABEu);
	EXPECT_EQ(out.settings, s.settings);
	EXPECT_EQ(out.peerList("192.0.2.1"), "192.0.2.1:19713,192.0.2.7:19714,192.0.2.8:19715");
}

TEST(TcvrLobbyCodec, StartRejectsBadLists)
{
	StartInfo s;
	s.peers = { { "a", 1 } };	// one player is no match
	StartInfo out;
	EXPECT_FALSE(decode(encode(s), out));
	s.peers = { { "a", 1 }, { "b", 2 } };
	s.localIndex = 2;			// outside the list
	EXPECT_FALSE(decode(encode(s), out));
}

TEST(TcvrLobbyCodec, BeaconAndQuery)
{
	Beacon b;
	b.hostId = 12345;
	b.appVersion = "1.0";
	b.gameId = "MK-5102050";
	b.hostName = "MAN";
	b.settings = { 1, 4, 60 };
	b.players = 3;
	b.lobbyPort = 40001;
	Beacon out;
	const auto bytes = encode(b);
	ASSERT_EQ(decodeDatagram(bytes.data(), bytes.size(), out), Datagram::Beacon);
	EXPECT_EQ(out.hostId, 12345u);
	EXPECT_EQ(out.hostName, "MAN");
	EXPECT_EQ(out.settings, b.settings);
	EXPECT_EQ(out.players, 3);
	EXPECT_EQ(out.lobbyPort, 40001);
	EXPECT_FALSE(out.started);
	const auto q = encodeQuery();
	EXPECT_EQ(decodeDatagram(q.data(), q.size(), out), Datagram::Query);
}

TEST(TcvrLobbyCodec, GarbageIsRefused)
{
	Beacon b;
	const uint8_t junk[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	EXPECT_EQ(decodeDatagram(junk, sizeof(junk), b), Datagram::Invalid);
	EXPECT_EQ(decodeDatagram(junk, 0, b), Datagram::Invalid);
	// a good beacon cut short, at every length
	b.lobbyPort = 5;
	b.hostName = "ABC";
	const auto bytes = encode(b);
	for (size_t n = 0; n < bytes.size(); n++)
		EXPECT_EQ(decodeDatagram(bytes.data(), n, b), Datagram::Invalid) << "length " << n;
	// the same for the lobby messages
	const auto state = encode(sampleState());
	for (size_t n = 0; n < state.size(); n++)
	{
		State s;
		std::vector<uint8_t> cut(state.begin(), state.begin() + n);
		EXPECT_FALSE(decode(cut, s)) << "length " << n;
	}
}

TEST(TcvrLobbyCodec, NamesAreCleaned)
{
	EXPECT_EQ(cleanName(""), "???");
	EXPECT_EQ(cleanName("A\x01" "B\n"), "AB");
	EXPECT_EQ(cleanName("ABCDEFGHIJKL"), "ABCDEFGH");
	Hello h;
	h.name = "X\x02Y";
	h.appVersion = "v";
	Hello out;
	ASSERT_TRUE(decode(encode(h), out));
	EXPECT_EQ(out.name, "XY");
}

TEST(TcvrLobbyCodec, FramesInPiecesAndTogether)
{
	std::vector<uint8_t> stream;
	for (auto f : { encodeFrame(MsgType::Ping), encodeFrame(MsgType::State, encode(sampleState())), encodeFrame(MsgType::Ready, encodeReady(true)) })
		stream.insert(stream.end(), f.begin(), f.end());
	// one byte at a time
	FrameParser parser;
	std::vector<FrameParser::Frame> frames;
	for (uint8_t b : stream)
		ASSERT_TRUE(parser.feed(&b, 1, frames));
	ASSERT_EQ(frames.size(), 3u);
	EXPECT_EQ(frames[0].type, MsgType::Ping);
	EXPECT_EQ(frames[1].type, MsgType::State);
	State s;
	EXPECT_TRUE(decode(frames[1].payload, s));
	// all at once
	FrameParser parser2;
	std::vector<FrameParser::Frame> frames2;
	ASSERT_TRUE(parser2.feed(stream.data(), stream.size(), frames2));
	EXPECT_EQ(frames2.size(), 3u);
}

TEST(TcvrLobbyCodec, BrokenStreamIsDetected)
{
	FrameParser parser;
	std::vector<FrameParser::Frame> frames;
	const uint8_t tooLong[] = { 0xff, 0xff, 1 };
	EXPECT_FALSE(parser.feed(tooLong, sizeof(tooLong), frames));
	FrameParser parser2;
	const uint8_t badType[] = { 1, 0, 99 };
	EXPECT_FALSE(parser2.feed(badType, sizeof(badType), frames));
	FrameParser parser3;
	const uint8_t empty[] = { 0, 0 };
	EXPECT_FALSE(parser3.feed(empty, sizeof(empty), frames));
}

// ---- real lobbies over loopback ---------------------------------------------------------------

namespace
{

Timing fast()
{
	Timing t;
	t.beaconMs = 100;
	t.visibleMs = 700;
	t.pingMs = 100;
	t.dropMs = 600;
	t.connectMs = 800;
	return t;
}

struct Net
{
	uint16_t port = nextPort();
	void prepare(Lobby& l)
	{
		l.setDiscoveryPort(port);
		// several lobbies in this one test program share the port: broadcast on the loopback network
		l.addBeaconTarget("127.255.255.255");
	}
};

// joins the first game the list shows
bool joinFirst(Lobby& joiner, const Identity& me)
{
	if (!waitFor([&] { return !joiner.games().empty(); }))
		return false;
	const FoundGame g = joiner.games().front();
	joiner.join(me, g.address, g.beacon.lobbyPort);
	return true;
}

}

TEST(TcvrLobby, HostIsFoundAndJoined)
{
	Net net;
	Lobby host(fast()), guest(fast());
	net.prepare(host);
	net.prepare(guest);
	std::string err;
	ASSERT_TRUE(host.host(ident("HOS", 19713), { 1, 6, 30 }, err)) << err;
	guest.startSearch();
	ASSERT_TRUE(waitFor([&] { return !guest.games().empty(); })) << "no game found";
	const FoundGame g = guest.games().front();
	EXPECT_EQ(g.beacon.hostName, "HOS");
	EXPECT_EQ(g.beacon.settings, (Settings { 1, 6, 30 }));
	EXPECT_EQ(g.beacon.players, 1);
	EXPECT_EQ(g.beacon.gameId, "MK-5102050");
	guest.join(ident("GUE", 19714), g.address, g.beacon.lobbyPort);
	ASSERT_TRUE(waitFor([&] { return guest.view().phase == Phase::InLobby; }));
	EXPECT_EQ(guest.view().mySlot, 1);
	ASSERT_TRUE(waitFor([&] { return host.view().state.count() == 2; }));
	ASSERT_TRUE(waitFor([&] { return guest.view().state.count() == 2; }));
	const State s = guest.view().state;
	EXPECT_EQ(s.players[0].name, "HOS");
	EXPECT_EQ(s.players[1].name, "GUE");
	EXPECT_EQ(s.players[1].ggpoPort, 19714);
	EXPECT_EQ(s.settings, (Settings { 1, 6, 30 }));
	EXPECT_EQ(s.players[1].address.empty(), false);
	// the beacon follows the player count
	ASSERT_TRUE(waitFor([&] { auto l = guest.games(); return !l.empty() && l.front().beacon.players == 2; }));
}

TEST(TcvrLobby, NoGameFound)
{
	Net net;
	Lobby guest(fast());
	net.prepare(guest);
	guest.startSearch();
	std::this_thread::sleep_for(std::chrono::milliseconds(400));
	EXPECT_TRUE(guest.games().empty());
}

TEST(TcvrLobby, GameDisappearsWhenHostLeaves)
{
	Net net;
	Lobby host(fast()), guest(fast());
	net.prepare(host);
	net.prepare(guest);
	std::string err;
	ASSERT_TRUE(host.host(ident("HOS", 19713), {}, err));
	guest.startSearch();
	ASSERT_TRUE(waitFor([&] { return !guest.games().empty(); }));
	host.leave();
	EXPECT_TRUE(waitFor([&] { return guest.games().empty(); }, 2500)) << "the game stays in the list";
}

TEST(TcvrLobby, EnterAddress)
{
	Net net;
	Lobby host(fast()), guest(fast());
	net.prepare(host);
	net.prepare(guest);
	std::string err;
	ASSERT_TRUE(host.host(ident("HOS", 19713), {}, err));
	// the joiner hears nothing from the broadcast here (its list is cleared again), and asks one address
	guest.startSearch();
	guest.query("127.0.0.1");
	ASSERT_TRUE(waitFor([&] { return !guest.games().empty(); }));
}

TEST(TcvrLobby, RefusedForOtherVersionAndDisc)
{
	Net net;
	Lobby host(fast()), a(fast()), b(fast());
	net.prepare(host);
	net.prepare(a);
	net.prepare(b);
	std::string err;
	ASSERT_TRUE(host.host(ident("HOS", 19713, "1.0", 1), {}, err));
	a.startSearch();
	ASSERT_TRUE(joinFirst(a, ident("AAA", 19714, "2.0", 1)));
	ASSERT_TRUE(waitFor([&] { return a.view().phase == Phase::Refused; }));
	EXPECT_EQ(a.view().refuse, RefuseReason::Version);
	EXPECT_STREQ(a.view().message.c_str(), refuseText(RefuseReason::Version));
	b.startSearch();
	ASSERT_TRUE(joinFirst(b, ident("BBB", 19715, "1.0", 2)));
	ASSERT_TRUE(waitFor([&] { return b.view().phase == Phase::Refused; }));
	EXPECT_EQ(b.view().refuse, RefuseReason::Disc);
	EXPECT_EQ(host.view().state.count(), 1);
}

TEST(TcvrLobby, FullAtFourPlayers)
{
	Net net;
	Lobby host(fast());
	Lobby g[4] = { Lobby(fast()), Lobby(fast()), Lobby(fast()), Lobby(fast()) };
	net.prepare(host);
	for (Lobby& l : g)
		net.prepare(l);
	std::string err;
	ASSERT_TRUE(host.host(ident("HOS", 19713), {}, err));
	for (int i = 0; i < 3; i++)
	{
		g[i].startSearch();
		ASSERT_TRUE(joinFirst(g[i], ident(std::string("P") + char('1' + i), 19714 + i)));
		ASSERT_TRUE(waitFor([&] { return g[i].view().phase == Phase::InLobby; }));
		EXPECT_EQ(g[i].view().mySlot, i + 1);
	}
	ASSERT_TRUE(waitFor([&] { return host.view().state.count() == 4; }));
	g[3].startSearch();
	ASSERT_TRUE(joinFirst(g[3], ident("P4", 19720)));
	ASSERT_TRUE(waitFor([&] { return g[3].view().phase == Phase::Refused; }));
	EXPECT_EQ(g[3].view().refuse, RefuseReason::Full);
}

TEST(TcvrLobby, ReadyAndStartWithFourPlayers)
{
	Net net;
	Lobby host(fast());
	Lobby g[3] = { Lobby(fast()), Lobby(fast()), Lobby(fast()) };
	net.prepare(host);
	for (Lobby& l : g)
		net.prepare(l);
	std::string err;
	ASSERT_TRUE(host.host(ident("HOS", 19713), { 2, 2, 60 }, err));
	std::string why;
	EXPECT_FALSE(host.canStart(why));
	EXPECT_FALSE(host.start(2, err));	// alone
	for (int i = 0; i < 3; i++)
	{
		g[i].startSearch();
		ASSERT_TRUE(joinFirst(g[i], ident(std::string("P") + char('1' + i), 19714 + i)));
		ASSERT_TRUE(waitFor([&] { return g[i].view().phase == Phase::InLobby; }));
	}
	ASSERT_TRUE(waitFor([&] { return host.view().state.count() == 4; }));
	EXPECT_FALSE(host.start(2, err)) << "started with players not ready";
	for (Lobby& l : g)
		l.setReady(true);
	ASSERT_TRUE(waitFor([&] { return host.canStart(why); })) << why;
	// ready marks reach everybody
	ASSERT_TRUE(waitFor([&] {
		const State s = g[0].view().state;
		return s.players[1].ready && s.players[2].ready && s.players[3].ready;
	}));
	g[1].setReady(false);
	ASSERT_TRUE(waitFor([&] { return !host.canStart(why); }));
	g[1].setReady(true);
	ASSERT_TRUE(waitFor([&] { return host.canStart(why); }));
	ASSERT_TRUE(host.start(3, err)) << err;
	EXPECT_EQ(host.view().phase, Phase::Started);
	for (int i = 0; i < 3; i++)
	{
		ASSERT_TRUE(waitFor([&] { return g[i].view().phase == Phase::Started; })) << "joiner " << i;
		const StartInfo s = g[i].view().start;
		EXPECT_EQ(s.localIndex, i + 1);
		EXPECT_EQ(s.peers.size(), 4u);
		EXPECT_EQ(s.inputDelay, 3);
		EXPECT_EQ(s.settings, (Settings { 2, 2, 60 }));
		EXPECT_EQ(s.peers[0].address, "");
		EXPECT_EQ(s.peers[0].port, 19713);
		for (int k = 1; k < 4; k++)
			EXPECT_EQ(s.peers[k].port, 19713 + k) << "peer " << k;
		EXPECT_EQ(s.sessionId, host.view().start.sessionId);
	}
	EXPECT_EQ(host.view().start.localIndex, 0);
	// a late joiner is refused: the game has started
	Lobby late(fast());
	net.prepare(late);
	late.join(ident("LAT", 19730), "127.0.0.1", host.view().lobbyPort);
	ASSERT_TRUE(waitFor([&] { return late.view().phase == Phase::Refused; }));
	EXPECT_EQ(late.view().refuse, RefuseReason::Started);
}

TEST(TcvrLobby, HolesInSlotsAreClosedAtStart)
{
	Net net;
	Lobby host(fast()), a(fast()), b(fast());
	net.prepare(host);
	net.prepare(a);
	net.prepare(b);
	std::string err;
	ASSERT_TRUE(host.host(ident("HOS", 19713), {}, err));
	a.startSearch();
	b.startSearch();
	ASSERT_TRUE(joinFirst(a, ident("AAA", 19714)));
	ASSERT_TRUE(waitFor([&] { return host.view().state.count() == 2; }));
	ASSERT_TRUE(joinFirst(b, ident("BBB", 19715)));
	ASSERT_TRUE(waitFor([&] { return host.view().state.count() == 3; }));
	a.leave();									// slot 1 is free again
	ASSERT_TRUE(waitFor([&] { return host.view().state.count() == 2; }));
	EXPECT_FALSE(host.view().state.players[1].present);
	b.setReady(true);
	ASSERT_TRUE(waitFor([&] { std::string w; return host.canStart(w); }));
	ASSERT_TRUE(host.start(2, err));
	ASSERT_TRUE(waitFor([&] { return b.view().phase == Phase::Started; }));
	EXPECT_EQ(b.view().start.peers.size(), 2u);
	EXPECT_EQ(b.view().start.localIndex, 1);		// player 2, not slot 3
	EXPECT_EQ(b.view().start.peers[1].port, 19715);
}

TEST(TcvrLobby, SettingsChangeReachesJoiners)
{
	Net net;
	Lobby host(fast()), guest(fast());
	net.prepare(host);
	net.prepare(guest);
	std::string err;
	ASSERT_TRUE(host.host(ident("HOS", 19713), { 0, 0, 10 }, err));
	guest.startSearch();
	ASSERT_TRUE(joinFirst(guest, ident("GUE", 19714)));
	ASSERT_TRUE(waitFor([&] { return guest.view().phase == Phase::InLobby; }));
	host.setSettings({ 2, 7, 120 });
	ASSERT_TRUE(waitFor([&] { return guest.view().state.settings == (Settings { 2, 7, 120 }); }));
	ASSERT_TRUE(waitFor([&] { auto l = guest.games(); return !l.empty() && l.front().beacon.settings == (Settings { 2, 7, 120 }); }));
}

TEST(TcvrLobby, HostLeavingIsReported)
{
	Net net;
	Lobby host(fast()), guest(fast());
	net.prepare(host);
	net.prepare(guest);
	std::string err;
	ASSERT_TRUE(host.host(ident("HOS", 19713), {}, err));
	guest.startSearch();
	ASSERT_TRUE(joinFirst(guest, ident("GUE", 19714)));
	ASSERT_TRUE(waitFor([&] { return guest.view().phase == Phase::InLobby; }));
	host.leave();
	ASSERT_TRUE(waitFor([&] { return guest.view().phase == Phase::HostLeft; }));
	EXPECT_EQ(guest.view().message, "The host left");
	EXPECT_EQ(host.view().phase, Phase::Idle);
}

TEST(TcvrLobby, JoinerLeavingFreesItsSlot)
{
	Net net;
	Lobby host(fast()), guest(fast());
	net.prepare(host);
	net.prepare(guest);
	std::string err;
	ASSERT_TRUE(host.host(ident("HOS", 19713), {}, err));
	guest.startSearch();
	ASSERT_TRUE(joinFirst(guest, ident("GUE", 19714)));
	ASSERT_TRUE(waitFor([&] { return host.view().state.count() == 2; }));
	guest.leave();
	EXPECT_TRUE(waitFor([&] { return host.view().state.count() == 1; }));
	EXPECT_EQ(guest.view().phase, Phase::Idle);
}

TEST(TcvrLobby, SilentPeerIsDropped)
{
	// a joiner whose process freezes (no pings): the host drops it after dropMs
	Net net;
	Timing slow = fast();
	slow.pingMs = 100000;		// the joiner never pings
	Lobby host(fast()), guest(slow);
	net.prepare(host);
	net.prepare(guest);
	std::string err;
	ASSERT_TRUE(host.host(ident("HOS", 19713), {}, err));
	guest.startSearch();
	ASSERT_TRUE(joinFirst(guest, ident("GUE", 19714)));
	ASSERT_TRUE(waitFor([&] { return host.view().state.count() == 2; }));
	EXPECT_TRUE(waitFor([&] { return host.view().state.count() == 1; }, 3000));
}

TEST(TcvrLobby, KeepAliveHoldsAnIdleLobby)
{
	Net net;
	Lobby host(fast()), guest(fast());
	net.prepare(host);
	net.prepare(guest);
	std::string err;
	ASSERT_TRUE(host.host(ident("HOS", 19713), {}, err));
	guest.startSearch();
	ASSERT_TRUE(joinFirst(guest, ident("GUE", 19714)));
	ASSERT_TRUE(waitFor([&] { return host.view().state.count() == 2; }));
	// longer than the drop time, with nothing happening
	std::this_thread::sleep_for(std::chrono::milliseconds(1500));
	EXPECT_EQ(host.view().state.count(), 2);
	EXPECT_EQ(guest.view().phase, Phase::InLobby);
}

TEST(TcvrLobby, NobodyThereIsReported)
{
	Lobby guest(fast());
	guest.join(ident("GUE", 19714), "127.0.0.1", 1);		// nothing listens on port 1
	ASSERT_TRUE(waitFor([&] { return guest.view().phase == Phase::Failed; }));
	EXPECT_FALSE(guest.view().message.empty());
	guest.join(ident("GUE", 19714), "not an address", 1);
	EXPECT_EQ(guest.view().phase, Phase::Failed);
}

TEST(TcvrLobby, GarbageOnTheLobbyPortIsDropped)
{
	Net net;
	Lobby host(fast());
	net.prepare(host);
	std::string err;
	ASSERT_TRUE(host.host(ident("HOS", 19713), {}, err));
	asio::io_context io;
	asio::ip::tcp::socket s(io);
	s.connect(asio::ip::tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), host.view().lobbyPort));
	const uint8_t junk[] = { 0xff, 0xff, 0xff, 0xff, 1, 2, 3 };
	asio::write(s, asio::buffer(junk));
	// the host closes it: a read ends
	asio::error_code ec;
	uint8_t b[16];
	s.read_some(asio::buffer(b), ec);
	EXPECT_TRUE(ec == asio::error::eof || ec == asio::error::connection_reset);
	EXPECT_EQ(host.view().state.count(), 1);
}

// ---- the address a host announces ----------------------------------------------------------

namespace
{

NetInterface nif(const std::string& name, const std::string& address, bool gateway = false,
		const std::string& description = "")
{
	NetInterface n;
	n.name = name;
	n.description = description;
	n.address = address;
	n.gateway = gateway;
	return n;
}

}

TEST(TcvrLobbyAddress, UnusableAddresses)
{
	EXPECT_LT(lanScore(nif("lo", "127.0.0.1")), 0);
	NetInterface lo = nif("Loopback Pseudo-Interface 1", "203.0.113.5");
	lo.loopback = true;
	EXPECT_LT(lanScore(lo), 0);
	EXPECT_LT(lanScore(nif("Wi-Fi", "169.254.12.3")), 0);	// no DHCP lease, leakscan:allow (link-local example)
	EXPECT_LT(lanScore(nif("Wi-Fi", "0.0.0.0")), 0);
	EXPECT_LT(lanScore(nif("Wi-Fi", "not an address")), 0);
	NetInterface down = nif("Wi-Fi", "192.0.2.20", true);
	down.up = false;
	EXPECT_LT(lanScore(down), 0);
	EXPECT_EQ(pickLanAddress({}), "");
	EXPECT_EQ(pickLanAddress({ nif("lo", "127.0.0.1"), down }), "");
}

// the bug found on the PC: the Hyper-V "Default Switch" (no gateway) was announced instead of Wi-Fi
TEST(TcvrLobbyAddress, WindowsSkipsHyperVDefaultSwitch)
{
	const std::string hv = "Hyper-V Virtual Ethernet Adapter";
	NetInterface wifi = nif("Wi-Fi", "192.0.2.20", true, "Wireless LAN Card");
	wifi.wireless = true;
	EXPECT_EQ(pickLanAddress({ nif("vEthernet (Default Switch)", "198.51.100.1", false, hv), wifi }), "192.0.2.20");
	EXPECT_EQ(pickLanAddress({ wifi, nif("vEthernet (Default Switch)", "198.51.100.1", false, hv) }), "192.0.2.20");
	// WSL and VirtualBox host-only too
	EXPECT_EQ(pickLanAddress({ nif("vEthernet (WSL)", "198.51.100.2", false, hv),
			nif("VirtualBox Host-Only Network", "198.51.100.3", false, "VirtualBox Host-Only Ethernet Adapter"),
			wifi }), "192.0.2.20");
}

// a Hyper-V external switch bridged to the LAN is the real LAN: it holds the gateway
TEST(TcvrLobbyAddress, WindowsKeepsBridgedHyperVSwitch)
{
	const std::string hv = "Hyper-V Virtual Ethernet Adapter";
	EXPECT_EQ(pickLanAddress({ nif("vEthernet (Default Switch)", "198.51.100.1", false, hv),
			nif("vEthernet (Bridge)", "203.0.113.3", true, hv + " #2") }), "203.0.113.3");
	// but a physical adapter with its own gateway is still better
	NetInterface eth = nif("Ethernet", "192.0.2.7", true, "USB Ethernet");
	eth.wired = true;
	EXPECT_EQ(pickLanAddress({ nif("vEthernet (Bridge)", "203.0.113.3", true, hv + " #2"), eth }), "192.0.2.7");
}

TEST(TcvrLobbyAddress, VpnLoses)
{
	NetInterface vpn = nif("WireGuard Tunnel", "203.0.113.8", true, "WireGuard Tunnel");
	NetInterface wifi = nif("Wi-Fi", "192.0.2.20", true);
	wifi.wireless = true;
	EXPECT_EQ(pickLanAddress({ vpn, wifi }), "192.0.2.20");
	// Android: a full-tunnel VPN holds the default route, Wi-Fi is still the room's network
	EXPECT_EQ(pickLanAddress({ nif("tun0", "203.0.113.8", true), nif("wlan0", "192.0.2.30") }), "192.0.2.30");
	// only a VPN: better than nothing
	EXPECT_EQ(pickLanAddress({ nif("lo", "127.0.0.1"), nif("tun0", "203.0.113.8", true) }), "203.0.113.8");
}

TEST(TcvrLobbyAddress, AndroidPrefersWifi)
{
	EXPECT_EQ(pickLanAddress({ nif("lo", "127.0.0.1"), nif("rmnet_data0", "203.0.113.45"), nif("wlan0", "192.0.2.30", true) }),
			"192.0.2.30");
	EXPECT_EQ(pickLanAddress({ nif("p2p0", "198.51.100.49"), nif("wlan0", "192.0.2.30") }), "192.0.2.30");
	// Linux PC: Docker bridge versus the real adapter
	EXPECT_EQ(pickLanAddress({ nif("docker0", "198.51.100.17"), nif("enp3s0", "192.0.2.40", true) }), "192.0.2.40");
}

TEST(TcvrLobbyAddress, TieGoesToLowerMetric)
{
	NetInterface a = nif("Ethernet", "192.0.2.10", true);
	NetInterface b = nif("Wi-Fi", "192.0.2.11", true);
	a.wired = true;
	b.wireless = true;
	a.metric = 35;
	b.metric = 25;
	EXPECT_EQ(pickLanAddress({ a, b }), "192.0.2.11");
	b.metric = 50;
	EXPECT_EQ(pickLanAddress({ a, b }), "192.0.2.10");
}

// this computer: whatever it picks is one of its own usable addresses
TEST(TcvrLobbyAddress, ThisComputer)
{
	const std::vector<NetInterface> list = listInterfaces();
	const std::string picked = pickLanAddress(list);
	for (const NetInterface& n : list)
		std::printf("  %-40s %-16s gw=%d score=%d\n", n.name.c_str(), n.address.c_str(), n.gateway, lanScore(n));
	if (!picked.empty())
	{
		bool found = false;
		for (const NetInterface& n : list)
			found |= n.address == picked && lanScore(n) >= 0;
		EXPECT_TRUE(found);
	}
}
