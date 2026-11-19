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
	s.players[0] = { true, "AAA", true, "192.0.2.6", 19713, 40001 };
	s.players[2] = { true, "ZOE", false, "", 19714, 40002 };
	s.lobbyId = 0x12345678;
	s.hostSlot = 2;
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

TEST(TcvrLobbyCodec, HelloRejoinRoundTrip)
{
	Hello h;
	h.name = "ZOE";
	h.appVersion = "v";
	h.ggpoPort = 19714;
	h.lobbyPort = 50123;
	h.lobbyId = 0xDEADBEEF;
	h.slot = 2;
	h.ready = true;
	Hello out;
	ASSERT_TRUE(decode(encode(h), out));
	EXPECT_EQ(out.lobbyPort, 50123);
	EXPECT_EQ(out.lobbyId, 0xDEADBEEFu);
	EXPECT_EQ(out.slot, 2);
	EXPECT_TRUE(out.ready);
	// a fresh join: no game id, no slot
	Hello fresh;
	ASSERT_TRUE(decode(encode(Hello()), fresh));
	EXPECT_EQ(fresh.lobbyId, 0u);
	EXPECT_EQ(fresh.slot, NoSlot);
	EXPECT_FALSE(fresh.ready);
	// an older app's HELLO (protocol 1, shorter) still decodes: it is then refused for its version
	Hello old;
	old.protocol = 1;
	auto bytes = encode(old);
	bytes.resize(bytes.size() - 8);
	Hello oldOut;
	ASSERT_TRUE(decode(bytes, oldOut));
	EXPECT_EQ(oldOut.protocol, 1);
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
	EXPECT_EQ(out.players[2].address, "");
	EXPECT_EQ(out.players[0].address, "192.0.2.6");
	EXPECT_EQ(out.players[2].ggpoPort, 19714);
	EXPECT_TRUE(out.players[0].ready);
	EXPECT_FALSE(out.players[2].ready);
	EXPECT_EQ(out.lobbyId, 0x12345678u);
	EXPECT_EQ(out.hostSlot, 2);
	EXPECT_EQ(out.players[0].lobbyPort, 40001);
	EXPECT_EQ(out.players[2].lobbyPort, 40002);
	State bad = s;
	bad.hostSlot = 4;
	EXPECT_FALSE(decode(encode(bad), out));
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
	b.hostName = "AAA";
	b.settings = { 1, 4, 60 };
	b.players = 3;
	b.lobbyPort = 40001;
	Beacon out;
	const auto bytes = encode(b);
	ASSERT_EQ(decodeDatagram(bytes.data(), bytes.size(), out), Datagram::Beacon);
	EXPECT_EQ(out.hostId, 12345u);
	EXPECT_EQ(out.hostName, "AAA");
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
	t.handoverMs = 1500;
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

TEST(TcvrLobby, HostLeavingHandsTheGameToTheOnlyJoiner)
{
	Net net;
	Lobby host(fast()), guest(fast()), late(fast());
	net.prepare(host);
	net.prepare(guest);
	net.prepare(late);
	std::string err;
	ASSERT_TRUE(host.host(ident("HOS", 19713), { 2, 4, 40 }, err));
	guest.startSearch();
	ASSERT_TRUE(joinFirst(guest, ident("GUE", 19714)));
	ASSERT_TRUE(waitFor([&] { return guest.view().phase == Phase::InLobby; }));
	const uint32_t id = guest.view().state.lobbyId;
	EXPECT_NE(id, 0u);
	host.leave();
	// the joiner hosts the same game, alone
	ASSERT_TRUE(waitFor([&] { return guest.view().role == Role::Host && guest.view().phase == Phase::InLobby; }));
	const View v = guest.view();
	EXPECT_EQ(v.mySlot, 1);
	EXPECT_EQ(v.state.hostSlot, 1);
	EXPECT_EQ(v.state.lobbyId, id);
	EXPECT_EQ(v.state.count(), 1);
	EXPECT_EQ(v.state.settings, (Settings { 2, 4, 40 }));
	EXPECT_EQ(v.message, "HOS left: you are the host now");
	EXPECT_EQ(host.view().phase, Phase::Idle);
	// and announces it: a new player finds it (same game id, the new host's name) and joins
	late.startSearch();
	ASSERT_TRUE(waitFor([&] { auto l = late.games(); return !l.empty() && l.front().beacon.hostName == "GUE"; }));
	EXPECT_EQ(late.games().front().hostId, id);
	ASSERT_TRUE(joinFirst(late, ident("LAT", 19715)));
	ASSERT_TRUE(waitFor([&] { return late.view().phase == Phase::InLobby; }));
	EXPECT_EQ(late.view().mySlot, 0);		// the free slot
	EXPECT_EQ(late.view().state.hostSlot, 1);
	ASSERT_TRUE(waitFor([&] { return guest.view().state.count() == 2; }));
}

TEST(TcvrLobby, HostLeavingWithFourPlayers)
{
	// the next player in slot order hosts; the others come back to their own slots, ready marks kept
	Net net;
	Lobby host(fast());
	Lobby g[3] = { Lobby(fast()), Lobby(fast()), Lobby(fast()) };
	net.prepare(host);
	for (Lobby& l : g)
		net.prepare(l);
	std::string err;
	ASSERT_TRUE(host.host(ident("HOS", 19713), { 1, 3, 30 }, err));
	for (int i = 0; i < 3; i++)
	{
		g[i].startSearch();
		ASSERT_TRUE(joinFirst(g[i], ident(std::string("P") + char('1' + i), 19714 + i)));
		ASSERT_TRUE(waitFor([&] { return g[i].view().phase == Phase::InLobby; }));
	}
	ASSERT_TRUE(waitFor([&] { return g[2].view().state.count() == 4; }));
	g[2].setReady(true);
	ASSERT_TRUE(waitFor([&] { return g[1].view().state.players[3].ready; }));
	const uint32_t id = g[0].view().state.lobbyId;
	host.leave();
	ASSERT_TRUE(waitFor([&] { return g[0].view().role == Role::Host; }));
	ASSERT_TRUE(waitFor([&] {
		return g[1].view().phase == Phase::InLobby && g[2].view().phase == Phase::InLobby && g[0].view().state.count() == 3
			&& g[1].view().state.hostSlot == 1 && g[2].view().state.count() == 3;
	}, 4000));
	for (int i = 0; i < 3; i++)
	{
		const View v = g[i].view();
		EXPECT_EQ(v.mySlot, i + 1) << "player " << i;
		EXPECT_EQ(v.state.lobbyId, id);
		EXPECT_EQ(v.state.hostSlot, 1);
		EXPECT_FALSE(v.state.players[0].present);
		EXPECT_EQ(v.state.players[1].name, "P1");
		EXPECT_EQ(v.state.players[2].name, "P2");
		EXPECT_EQ(v.state.players[3].name, "P3");
		EXPECT_EQ(v.state.settings, (Settings { 1, 3, 30 }));
	}
	EXPECT_EQ(g[1].view().role, Role::Joiner);
	EXPECT_EQ(g[1].view().message, "The host left: P1 is the new host");
	EXPECT_FALSE(g[0].view().state.players[2].ready);
	EXPECT_TRUE(g[0].view().state.players[3].ready);		// kept
	// the new host starts the match: players in slot order, the new host first
	std::string why;
	EXPECT_FALSE(g[0].canStart(why));
	g[1].setReady(true);
	ASSERT_TRUE(waitFor([&] { return g[0].canStart(why); })) << why;
	ASSERT_TRUE(g[0].start(2, err)) << err;
	EXPECT_EQ(g[0].view().start.localIndex, 0);
	for (int i = 1; i < 3; i++)
	{
		ASSERT_TRUE(waitFor([&] { return g[i].view().phase == Phase::Started; })) << "joiner " << i;
		const StartInfo s = g[i].view().start;
		EXPECT_EQ(s.localIndex, i);
		ASSERT_EQ(s.peers.size(), 3u);
		EXPECT_EQ(s.peers[0].address, "");			// the host: the address it was joined by
		EXPECT_EQ(s.peers[0].port, 19714);
		EXPECT_EQ(s.peers[1].port, 19715);
		EXPECT_EQ(s.peers[2].port, 19716);
		const std::string ha = g[i].view().hostAddress;
		EXPECT_FALSE(ha.empty());
		EXPECT_EQ(s.peerList(ha).substr(0, ha.size() + 6), ha + ":19714");
	}
}

TEST(TcvrLobby, HostVanishingHandsOver)
{
	// no LEAVE: the host's process is gone (its sockets close)
	Net net;
	auto host = std::make_unique<Lobby>(fast());
	Lobby a(fast()), b(fast());
	net.prepare(*host);
	net.prepare(a);
	net.prepare(b);
	std::string err;
	ASSERT_TRUE(host->host(ident("HOS", 19713), {}, err));
	a.startSearch();
	b.startSearch();
	ASSERT_TRUE(joinFirst(a, ident("AAA", 19714)));
	ASSERT_TRUE(waitFor([&] { return a.view().phase == Phase::InLobby; }));
	ASSERT_TRUE(joinFirst(b, ident("BBB", 19715)));
	ASSERT_TRUE(waitFor([&] { return b.view().state.count() == 3; }));
	host.reset();
	ASSERT_TRUE(waitFor([&] { return a.view().role == Role::Host && b.view().phase == Phase::InLobby && b.view().state.hostSlot == 1; }, 4000));
	EXPECT_EQ(b.view().mySlot, 2);
	EXPECT_EQ(a.view().state.count(), 2);
}

TEST(TcvrLobby, HostSilentHandsOver)
{
	// the host's network goes quiet (no pings): the joiners drop it after dropMs and hand over
	Net net;
	Timing quiet = fast();
	quiet.pingMs = 100000;
	Lobby host(quiet), a(fast()), b(fast());
	net.prepare(host);
	net.prepare(a);
	net.prepare(b);
	std::string err;
	ASSERT_TRUE(host.host(ident("HOS", 19713), {}, err));
	a.startSearch();
	b.startSearch();
	ASSERT_TRUE(joinFirst(a, ident("AAA", 19714)));
	ASSERT_TRUE(waitFor([&] { return a.view().phase == Phase::InLobby; }));
	ASSERT_TRUE(joinFirst(b, ident("BBB", 19715)));
	ASSERT_TRUE(waitFor([&] { return b.view().state.count() == 3; }));
	ASSERT_TRUE(waitFor([&] { return a.view().role == Role::Host && b.view().phase == Phase::InLobby && b.view().state.hostSlot == 1; }, 4000));
	EXPECT_EQ(b.view().mySlot, 2);
}

TEST(TcvrLobby, SlotKeptOnlyForAWhile)
{
	// host and player 3 gone at once: player 2 hosts and keeps slot 2 for player 3, then frees it.
	// Player 3 is a bare connection here, so it surely never comes back.
	Net net;
	auto host = std::make_unique<Lobby>(fast());
	Lobby a(fast());
	net.prepare(*host);
	net.prepare(a);
	std::string err;
	ASSERT_TRUE(host->host(ident("HOS", 19713), {}, err));
	a.startSearch();
	ASSERT_TRUE(joinFirst(a, ident("AAA", 19714)));
	ASSERT_TRUE(waitFor([&] { return a.view().phase == Phase::InLobby; }));
	asio::io_context io;
	asio::ip::tcp::socket b(io);
	b.connect(asio::ip::tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), host->view().lobbyPort));
	Hello h;
	h.name = "BBB";
	h.appVersion = "1.0";
	h.disc.fill(1);
	h.ggpoPort = 19715;
	h.lobbyPort = 1;		// nothing there
	asio::write(b, asio::buffer(encodeFrame(MsgType::Hello, encode(h))));
	ASSERT_TRUE(waitFor([&] { return a.view().state.count() == 3; }));
	host.reset();
	ASSERT_TRUE(waitFor([&] { return a.view().role == Role::Host; }));
	EXPECT_EQ(a.view().state.count(), 2);		// BBB's slot, kept
	EXPECT_TRUE(a.view().state.players[2].present);
	std::string why;
	EXPECT_FALSE(a.canStart(why));
	EXPECT_EQ(why, "Waiting for the players to come back");
	ASSERT_TRUE(waitFor([&] { return a.view().state.count() == 1; }, 3000));
	EXPECT_FALSE(a.canStart(why));
	EXPECT_EQ(why, "Waiting for another player");
}

TEST(TcvrLobby, NextInLineGoneToo)
{
	// host and player 2 (the next host) gone at once: player 3 tries player 2, then hosts itself
	Net net;
	auto host = std::make_unique<Lobby>(fast());
	auto a = std::make_unique<Lobby>(fast());
	Lobby b(fast());
	net.prepare(*host);
	net.prepare(*a);
	net.prepare(b);
	std::string err;
	ASSERT_TRUE(host->host(ident("HOS", 19713), {}, err));
	a->startSearch();
	b.startSearch();
	ASSERT_TRUE(joinFirst(*a, ident("AAA", 19714)));
	ASSERT_TRUE(waitFor([&] { return a->view().phase == Phase::InLobby; }));
	ASSERT_TRUE(joinFirst(b, ident("BBB", 19715)));
	ASSERT_TRUE(waitFor([&] { return b.view().state.count() == 3; }));
	a.reset();
	host.reset();
	ASSERT_TRUE(waitFor([&] { return b.view().role == Role::Host && b.view().phase == Phase::InLobby; }, 5000));
	EXPECT_EQ(b.view().state.hostSlot, 2);
	EXPECT_EQ(b.view().mySlot, 2);
}

TEST(TcvrLobby, NoHandoverOnceStarted)
{
	// after START the lobby is done: the netplay deals with a player who leaves
	Net net;
	Lobby host(fast()), guest(fast());
	net.prepare(host);
	net.prepare(guest);
	std::string err;
	ASSERT_TRUE(host.host(ident("HOS", 19713), {}, err));
	guest.startSearch();
	ASSERT_TRUE(joinFirst(guest, ident("GUE", 19714)));
	ASSERT_TRUE(waitFor([&] { return guest.view().phase == Phase::InLobby; }));
	guest.setReady(true);
	ASSERT_TRUE(waitFor([&] { std::string w; return host.canStart(w); }));
	ASSERT_TRUE(host.start(2, err));
	ASSERT_TRUE(waitFor([&] { return guest.view().phase == Phase::Started; }));
	host.leave();
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	EXPECT_EQ(guest.view().phase, Phase::Started);
	EXPECT_EQ(guest.view().role, Role::Joiner);
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

// ---- the end of a match: back to the lobby page (task 3.5) -------------------------------------

TEST(TcvrLobby, MatchOverBringsEverybodyBackToTheLobby)
{
	Net net;
	Lobby host(fast()), a(fast()), b(fast());
	net.prepare(host);
	net.prepare(a);
	net.prepare(b);
	std::string err;
	ASSERT_TRUE(host.host(ident("HOS", 19713), { 0, 3, 15 }, err));
	a.startSearch();
	b.startSearch();
	ASSERT_TRUE(joinFirst(a, ident("AAA", 19714)));
	ASSERT_TRUE(waitFor([&] { return host.view().state.count() == 2; }));
	ASSERT_TRUE(joinFirst(b, ident("BBB", 19715)));
	ASSERT_TRUE(waitFor([&] { return host.view().state.count() == 3; }));
	a.setReady(true);
	b.setReady(true);
	ASSERT_TRUE(waitFor([&] { std::string w; return host.canStart(w); }));
	ASSERT_TRUE(host.start(2, err)) << err;
	ASSERT_TRUE(waitFor([&] { return a.view().phase == Phase::Started && b.view().phase == Phase::Started; }));
	// the match lasts longer than the lobby's drop time: the lobby stays connected through it
	std::this_thread::sleep_for(std::chrono::milliseconds(1500));
	EXPECT_EQ(host.view().state.count(), 3);
	EXPECT_EQ(a.view().phase, Phase::Started);
	// a joiner can't come in during the match
	Lobby late(fast());
	net.prepare(late);
	late.join(ident("LAT", 19730), "127.0.0.1", host.view().lobbyPort);
	ASSERT_TRUE(waitFor([&] { return late.view().phase == Phase::Refused; }));
	// every copy ends the match on its own (in any order)
	a.matchOver();
	host.matchOver();
	b.matchOver();
	for (Lobby *l : { &host, &a, &b })
	{
		ASSERT_TRUE(waitFor([&] { return l->view().phase == Phase::InLobby; }));
		EXPECT_EQ(l->view().start.peers.size(), 0u);
	}
	// the same players, nobody ready, the settings kept
	ASSERT_TRUE(waitFor([&] {
		const State s = a.view().state;
		return s.count() == 3 && !s.players[1].ready && !s.players[2].ready;
	}));
	EXPECT_EQ(a.view().state.players[0].name, "HOS");
	EXPECT_EQ(a.view().state.players[1].name, "AAA");
	EXPECT_EQ(a.view().state.players[2].name, "BBB");
	EXPECT_EQ(a.view().state.settings, (Settings { 0, 3, 15 }));
	EXPECT_EQ(a.view().mySlot, 1);
	std::string why;
	EXPECT_FALSE(host.canStart(why));
	// the game is announced again (not started), and a second match can start
	Lobby seeker(fast());
	net.prepare(seeker);
	seeker.startSearch();
	ASSERT_TRUE(waitFor([&] { auto l = seeker.games(); return !l.empty() && !l.front().beacon.started && l.front().beacon.players == 3; }));
	a.setReady(true);
	b.setReady(true);
	ASSERT_TRUE(waitFor([&] { return host.canStart(why); })) << why;
	ASSERT_TRUE(host.start(2, err)) << err;
	ASSERT_TRUE(waitFor([&] { return a.view().phase == Phase::Started && b.view().phase == Phase::Started; }));
	EXPECT_EQ(b.view().start.localIndex, 2);
}

TEST(TcvrLobby, HostGoneDuringTheMatch)
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
	guest.setReady(true);
	ASSERT_TRUE(waitFor([&] { std::string w; return host.canStart(w); }));
	ASSERT_TRUE(host.start(2, err));
	ASSERT_TRUE(waitFor([&] { return guest.view().phase == Phase::Started; }));
	host.leave();
	// the match is not ended by the lobby (the netplay decides), but afterwards the host is gone
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	EXPECT_EQ(guest.view().phase, Phase::Started);
	guest.matchOver();
	// afterwards the lobby is handed over, as before a match: the only other player hosts it now
	ASSERT_TRUE(waitFor([&] { return guest.view().phase == Phase::InLobby && guest.view().role == Role::Host; }));
	EXPECT_EQ(guest.view().message, "HOS left: you are the host now");
}

TEST(TcvrLobby, MatchOverOutsideAMatchDoesNothing)
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
	guest.setReady(true);
	ASSERT_TRUE(waitFor([&] { return host.view().state.players[1].ready; }));
	host.matchOver();
	guest.matchOver();
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	EXPECT_EQ(guest.view().phase, Phase::InLobby);
	EXPECT_TRUE(host.view().state.players[1].ready);
}
