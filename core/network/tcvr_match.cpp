/*
	tcvr: the multiplayer match, from the lobby to the game. See tcvr_match.h.

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "tcvr_match.h"
#include "cfg/option.h"
#include "emulator.h"
#include "input/udp_lightgun.h"
#include "log/Log.h"
#include "net_platform.h"
#include "reios/reios.h"
#include "stdclass.h"
#include "types.h"
#include "hw/sh4/sh4_mem.h"
#include "input/gamepad.h"
#include "network/ggpo.h"
#include "version.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <mutex>
#include <thread>

namespace tcvr::match
{

std::string (*restartHook)();
std::string (*saveNameHook)();

namespace
{

std::mutex mutex;
std::unique_ptr<lobby::Lobby> theOne;
bool begun;			// START handled: the game is being started again
bool active;		// this run of the game is a match
std::string cachedSaveName;
bool saveNameKnown;

constexpr int MarkerSeconds = 120;	// a start that takes longer than this is not a start
constexpr int StartTimeoutSeconds = 60;	// in the running app: the other players did not come

int nowSeconds()
{
	return (int)std::time(nullptr);
}

void saveInt(config::Option<int>& o, int v) { o.set(v); o.save(); }
void saveBool(config::Option<bool>& o, bool v) { o.set(v); o.save(); }

// netplay off in emu.cfg, as the player's own settings had it
void clearNetplay()
{
	saveBool(config::GGPOEnable, false);
	config::GGPOPeers.set(std::string());
	config::GGPOPeers.save();
	saveInt(config::GGPOPlayer, 0);
	saveInt(config::VrMpMarker, 0);
	config::VrMpNames.set(std::string());
	config::VrMpNames.save();
}
// A match that ends while its game runs: netplay is switched off only once the game is unloaded
// (reloadNow). Emulator::stop() ends the GGPO session only while GGPOEnable is on; switched off
// first, GGPO went on restarting the SH4 for its frames and the reset that follows failed
// ("Verify Failed: !ctx->CpuRunning", the host closed when it left a match).
std::atomic<bool> clearPending;

// The game is loaded again inside the running app (task X2: the app no longer restarts itself, which on
// some headsets came back behind the Quest's own screens). The render thread does it (reloadNow), between
// two headset frames; the headset keeps showing a panel meanwhile (panelText).
enum class Reload { None, IntoMatch, BackToLobby };
std::atomic<int> reloadWanted { (int)Reload::None };	// for the render thread
std::atomic<int> reloading { (int)Reload::None };		// until the game runs again
std::chrono::steady_clock::time_point reloadSince;
std::string endLine;									// why the last match ended, for the menu

std::string restart(Reload kind)
{
	WARN_LOG(NETWORK, "MP RESTART in the running app (%s)", kind == Reload::IntoMatch ? "into the match" : "back to the lobby");
	reloadSince = std::chrono::steady_clock::now();
	reloading = (int)kind;
	reloadWanted = (int)kind;
	return kind == Reload::IntoMatch ? "Starting the match..." : "Back to the lobby...";
}

std::string upper(const std::string& s)
{
	std::string out;
	for (char c : s)
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ')
			out += (char)std::toupper((unsigned char)c);
	return out;
}

// The players' names in match order (the lobby's slots, holes closed, as lobby::start orders them).
std::vector<std::string> matchNames;
std::mutex resultsMutex;
Results lastResults;		// (resultsMutex: written on the emulator's thread)

void startMatch(const lobby::View& v)
{
	matchNames.clear();
	for (const lobby::PlayerInfo& p : v.state.players)
		if (p.present)
			matchNames.push_back(p.name);
	{
		std::lock_guard<std::mutex> lock(resultsMutex);
		lastResults = Results();
	}
	endLine.clear();
	const lobby::StartInfo& info = v.start;
	const std::string hostAddress = v.role == lobby::Role::Host ? "127.0.0.1" : v.hostAddress;
	const std::string peers = info.peerList(hostAddress);
	NOTICE_LOG(NETWORK, "MP START as player %d of %d, delay %d", info.localIndex + 1, (int)info.peers.size(), info.inputDelay);
	config::GGPOPeers.set(peers);
	config::GGPOPeers.save();
	saveInt(config::GGPOPlayer, info.localIndex);
	saveInt(config::GGPODelay, info.inputDelay);
	saveInt(config::GGPOAnalogAxes, 2);
	saveBool(config::EnableUPnP, false);		// a LAN match needs no port mapping
	saveBool(config::NetworkEnable, false);
	saveBool(config::BattleCableEnable, false);
	saveBool(config::ActAsServer, false);
	saveBool(config::GGPOEnable, true);
	const lobby::Settings& st = v.state.settings;
	config::VrMpSetup.set(std::to_string(st.mode) + "," + std::to_string(st.room) + "," + std::to_string(st.points));
	config::VrMpSetup.save();
	// the names in player order (the slots in order, holes closed, as START lists the players)
	std::string names;
	for (const lobby::PlayerInfo& p : v.state.players)
		if (p.present)
			names += (names.empty() ? "" : ",") + p.name;
	config::VrMpNames.set(names);
	config::VrMpNames.save();
	config::VrMpEndText.set(std::string());
	config::VrMpEndText.save();
	saveInt(config::VrMpMarker, nowSeconds() + MarkerSeconds);
	restart(Reload::IntoMatch);
}

}	// namespace

lobby::Lobby& theLobby()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (!theOne)
	{
		theOne = std::make_unique<lobby::Lobby>();
		theOne->setBroadcastHook([](bool on) { enableNetworkBroadcast(on); });
		if (const char *port = std::getenv("TCVR_MP_DISCOVERY_PORT"))	// tests: two tests on one PC at once
			theOne->setDiscoveryPort((uint16_t)std::atoi(port));
	}
	return *theOne;
}

bool lobbyStarted()
{
	std::lock_guard<std::mutex> lock(mutex);
	return theOne != nullptr;
}

lobby::Identity identity()
{
	lobby::Identity id;
	id.name = playerName();
	id.appVersion = GIT_VERSION;
	id.gameId = settings.content.gameId;
	MD5Sum().add(&ip_meta, sizeof(ip_meta)).getDigest(id.disc.data());
	id.ggpoPort = 19713;
	if (const char *port = std::getenv("TCVR_MP_PORT"))		// tests: several copies on one PC
		id.ggpoPort = (uint16_t)std::atoi(port);
	return id;
}

std::string playerName()
{
	std::string name = upper(config::VrMpName.get());
	if (name.empty())
	{
		if (!saveNameKnown && saveNameHook != nullptr)
		{
			cachedSaveName = upper(saveNameHook());
			saveNameKnown = true;
		}
		name = cachedSaveName;
	}
	if (name.empty())
		name = "PLR";
	if (name.size() > 3)
		name.resize(3);
	return name;
}

void setPlayerName(const std::string& name)
{
	std::string n = upper(name);
	if (n.size() > 3)
		n.resize(3);
	config::VrMpName.set(n);
	config::VrMpName.save();
}

static std::atomic<bool> lostPeer;
static std::atomic<bool> resultsDone;		// the game's results page has been and gone (setupButtons)

// Whether this copy was the one away when the others went silent: its own app stood still for seconds
// (process frozen, headset asleep). Not the game's frames: while one copy is away the others' games
// wait for its inputs and stand still too. The app's own loop (poll(), every headset frame) goes on there.
static std::atomic<int64_t> lastOwnFrameMs;		// poll()'s last call in the match
static std::atomic<int64_t> awaySinceMs;		// when poll() came back after such a gap (0: none)
static std::atomic<bool> selfAway;
constexpr int64_t AwayMs = 5000;		// GGPO says "interrupted" after 2 s of silence: well past that

static int64_t steadyMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void checkSelfAway()
{
	const int64_t now = steadyMs();
	const int64_t last = lastOwnFrameMs, back = awaySinceMs;
	const bool standing = last != 0 && now - last > AwayMs;		// (still away: the network thread came first)
	const bool justBack = back != 0 && now - back < 20000;		// (back from away a moment ago)
	if ((standing || justBack) && !selfAway.exchange(true))
		NOTICE_LOG(NETWORK, "MP match: this copy's app stood still %s: it was away", standing ? "until now" : "a moment ago");
}

// poll(): the app's own loop is running (in a match)
static void noteOwnLoop()
{
	const int64_t now = steadyMs();
	const int64_t last = lastOwnFrameMs.exchange(now);
	if (last != 0 && now - last > AwayMs)
	{
		awaySinceMs = now;
		NOTICE_LOG(NETWORK, "MP match: this copy's app stood still for %.1f s", (now - last) / 1000.f);
	}
}

static std::string leftLine(const std::string& who)
{
	if (selfAway)
		return "The match is over: you were away too long";
	return who.empty() ? "The match is over: a player left" : "The match is over: " + who + " left";
}

// "X left the match" notices (3-4 players: the match goes on) and why the last other player went
static std::mutex noticeMutex;
static std::string lostWhy;
static std::string noticeText;
static int noticeId;
static std::chrono::steady_clock::time_point noticeUntil;

// Paused players (GGPO "connection interrupted"): the player number and since when, or none.
static std::mutex pausedMutex;
static std::vector<std::pair<int, std::chrono::steady_clock::time_point>> pausedPlayers;

void peerInterrupted(int player, bool interrupted)
{
	if (player < 0)
		return;
	if (interrupted && active)
		checkSelfAway();
	std::lock_guard<std::mutex> lock(pausedMutex);
	auto it = std::find_if(pausedPlayers.begin(), pausedPlayers.end(), [player](const auto& p) { return p.first == player; });
	if (interrupted && it == pausedPlayers.end())
		pausedPlayers.push_back({ player, std::chrono::steady_clock::now() });
	else if (!interrupted && it != pausedPlayers.end())
		pausedPlayers.erase(it);
}

static void clearPaused()
{
	std::lock_guard<std::mutex> lock(pausedMutex);
	pausedPlayers.clear();
}

// a player's name: the lobby's names in match order (this run), else the match's list kept in emu.cfg
static std::string nameOf(int player)
{
	if (player >= 0 && player < (int)matchNames.size() && !matchNames[player].empty())
		return matchNames[player];
	const std::string& list = config::VrMpNames.get();
	size_t start = 0;
	for (int i = 0; i < player && start != std::string::npos; i++)
	{
		start = list.find(',', start);
		if (start != std::string::npos)
			start++;
	}
	if (start != std::string::npos && start < list.size())
	{
		const std::string name = list.substr(start, list.find(',', start) - start);
		if (!name.empty())
			return name;
	}
	return "Player " + std::to_string(player + 1);
}

void playerLeft(int player, bool allGone)
{
	const std::string who = nameOf(player);
	{
		// (gone: no "paused" panel for that player any more)
		std::lock_guard<std::mutex> lock(pausedMutex);
		pausedPlayers.erase(std::remove_if(pausedPlayers.begin(), pausedPlayers.end(), [player](const auto& p) { return p.first == player; }),
				pausedPlayers.end());
	}
	if (!allGone)
	{
		// 3 or 4 players: the match goes on without that player (its pad neutral on every copy, ggpo.cpp)
		NOTICE_LOG(NETWORK, "MP match: player %d (%s) left, the match goes on", player + 1, who.c_str());
		std::lock_guard<std::mutex> lock(noticeMutex);
		noticeText = who + " left the match";
		noticeId++;
		noticeUntil = std::chrono::steady_clock::now() + std::chrono::seconds(6);
		return;
	}
	if (!active)
		return;
	checkSelfAway();
	NOTICE_LOG(NETWORK, "MP match: player %d (%s) left, nobody else is left: the match is over", player + 1, who.c_str());
	{
		std::lock_guard<std::mutex> lock(noticeMutex);
		lostWhy = selfAway ? leftLine(who)
				: ggpo::playerCount() <= 2 ? leftLine(who) : "The match is over: the other players left";
	}
	// the next poll() (or gameStopped, when the netplay's error stops the game first) ends the match: back
	// to the lobby in the running app, netplay off once the game is unloaded
	lostPeer = true;
}

std::string notice(int& id)
{
	std::lock_guard<std::mutex> lock(noticeMutex);
	id = noticeId;
	if (noticeText.empty() || std::chrono::steady_clock::now() > noticeUntil)
		return std::string();
	return noticeText;
}

static std::string takeLostWhy()
{
	std::lock_guard<std::mutex> lock(noticeMutex);
	std::string why;
	why.swap(lostWhy);
	return why;
}

std::string lastEnd();

std::string endText()
{
	const std::string t = lastEnd();
	return t.empty() ? "The match is over" : t;
}

// The end of a match, however it ends: netplay off, back to the lobby page. The game is unloaded after
// this (reloadNow, or gui_stop_game's own unload then afterUnload): netplay goes off then.
static std::string endMatch(const std::string& why)
{
	NOTICE_LOG(NETWORK, "MP match over: %s", why.c_str());
	active = false;
	lostPeer = false;
	resultsDone = false;
	selfAway = false;
	lastOwnFrameMs = 0;
	awaySinceMs = 0;
	clearPaused();
	{
		std::lock_guard<std::mutex> lock(mutex);
		begun = false;
	}
	clearPending = true;
	endLine = why;
	config::VrMpEndText.set(why);
	config::VrMpEndText.save();
	saveBool(config::VrMpReturn, true);
	if (lobbyStarted())
		theLobby().matchOver();
	return why;
}

void poll()
{
	if (active)
		noteOwnLoop();
	if (lostPeer.exchange(false) && active)
	{
		std::string why = takeLostWhy();
		if (why.empty())
			why = leftLine(std::string());
		NOTICE_LOG(NETWORK, "MP match: %s", why.c_str());
		endMatch(why);
		restart(Reload::BackToLobby);
		return;
	}
	if (resultsDone.exchange(false) && active)
	{
		endMatch("The match is over");
		restart(Reload::BackToLobby);
		return;
	}
	if (reloading != (int)Reload::None)
	{
		// the game runs again: the reload is done
		if (reloadWanted == (int)Reload::None && emu.running())
		{
			NOTICE_LOG(NETWORK, "MP: the game runs again (%.1f s)",
					std::chrono::duration<float>(std::chrono::steady_clock::now() - reloadSince).count());
			reloading = (int)Reload::None;
		}
	}
	std::unique_lock<std::mutex> lock(mutex);
	if (begun || !theOne)
		return;
	lobby::Lobby *l = theOne.get();
	lock.unlock();
	const lobby::View v = l->view();
	if (v.phase != lobby::Phase::Started)
		return;
	{
		std::lock_guard<std::mutex> g(mutex);
		if (begun)
			return;
		begun = true;
	}
	startMatch(v);
}

bool inMatch()
{
	return active && config::GGPOEnable;
}

std::string leaveMatch(const std::string& why)
{
	if (!active && reloading == (int)Reload::None)
		return "Not in a match";
	endMatch(why.empty() ? "You left the match" : why);
	return restart(Reload::BackToLobby);
}

bool reloadNow()
{
	if (reloadWanted.exchange((int)Reload::None) == (int)Reload::None)
		return false;
	if (clearPending.exchange(false))
	{
		// the match's game stops with netplay still on (its GGPO session ends with it), then netplay off
		// (unloadGame reads emu.cfg again: still the match's options until clearNetplay)
		emu.unloadGame();
		clearNetplay();
	}
	return true;
}

bool gameStopped(const std::string& message)
{
	// (a match already over, netplay still on until the unload: not a match any more)
	const bool match = active || reloading == (int)Reload::IntoMatch || (config::GGPOEnable && !clearPending);
	if (!match)
	{
		// already on the way back (a player left: poll() came first): the caller just loads the game
		if (reloading != (int)Reload::BackToLobby)
			return false;
		reloadWanted = (int)Reload::None;
		return true;
	}
	WARN_LOG(NETWORK, "MP: the game stopped during a match: %s", message.c_str());
	std::string who;
	{
		std::lock_guard<std::mutex> lock(pausedMutex);
		if (!pausedPlayers.empty())
			who = nameOf(pausedPlayers.front().first);
	}
	const std::string lost = takeLostWhy();		// (the last other player left: the netplay's error came first)
	const bool starting = !active || reloading == (int)Reload::IntoMatch;
	endMatch(starting ? "The match could not start" + (message.empty() ? std::string() : ": " + message)
			: !lost.empty() ? lost
			: !who.empty() || selfAway ? leftLine(who)
			: "The match is over: " + (message.empty() ? std::string("the network dropped") : message));
	// the caller loads the game again (single player); the panel says so meanwhile
	reloadSince = std::chrono::steady_clock::now();
	reloading = (int)Reload::BackToLobby;
	reloadWanted = (int)Reload::None;
	return true;
}

void afterUnload()
{
	if (clearPending.exchange(false))
		clearNetplay();
}

bool startTimedOut()
{
	return reloading == (int)Reload::IntoMatch && reloadWanted == (int)Reload::None && !emu.running()
			&& std::chrono::steady_clock::now() - reloadSince > std::chrono::seconds(StartTimeoutSeconds);
}

bool panelText(std::string& line, std::string& detail)
{
	const int r = reloading;
	if (r == (int)Reload::IntoMatch)
	{
		const float secs = std::chrono::duration<float>(std::chrono::steady_clock::now() - reloadSince).count();
		line = "Starting the match...";
		detail = secs < 8.f ? "Waiting for the other players" : "Waiting for the other players (" + std::to_string(StartTimeoutSeconds - (int)secs) + " s)";
		return true;
	}
	if (r == (int)Reload::BackToLobby)
	{
		line = endLine.empty() ? "The match is over" : endLine;
		detail = "Back to the lobby...";
		return true;
	}
	if (!inMatch())
		return false;
	std::lock_guard<std::mutex> lock(pausedMutex);
	if (pausedPlayers.empty())
		return false;
	const auto& p = pausedPlayers.front();
	// GGPO says "interrupted" after 2 s of silence and gives up after 10 s (ggpo.cpp)
	const int left = std::max(0, 10 - 2 - (int)std::chrono::duration<float>(std::chrono::steady_clock::now() - p.second).count());
	line = nameOf(p.first) + " paused the game";
	detail = "The match goes on when " + nameOf(p.first) + " is back. Otherwise it ends in " + std::to_string(left) + " s.";
	return true;
}

Results results()
{
	std::lock_guard<std::mutex> lock(resultsMutex);
	return lastResults;
}

std::string lastEnd()
{
	return endLine;
}

// The Dreamcast devices of a match: the same on every copy, or GGPO refuses the peers (its
// verification hashes what is plugged into each port). One controller with a VMU per player,
// nothing in the other ports; each player's own setup (emu.cfg) is left as it is: overridden
// options are not saved.
static void matchDevices()
{
	const std::string& peers = config::GGPOPeers.get();
	const int players = std::clamp((int)std::count(peers.begin(), peers.end(), ',') + 1, 2, 4);
	for (int port = 0; port < 4; port++)
	{
		const bool used = port < players;
		config::MapleMainDevices[port].override(used ? MDT_SegaController : MDT_None);
		config::MapleExpansionDevices[port][0].override(used ? MDT_SegaVMU : MDT_None);
		config::MapleExpansionDevices[port][1].override(MDT_None);
	}
	NOTICE_LOG(NETWORK, "MP match: %d controllers with a VMU each", players);
}

static void resetWatch();

void gate()
{
	const int marker = config::VrMpMarker;
	if (marker > 0)
	{
		if (nowSeconds() <= marker && !config::GGPOPeers.get().empty() && config::GGPOEnable)
		{
			active = true;
			saveInt(config::VrMpMarker, -1);		// running: any later load that finds this is a left-over
			NOTICE_LOG(NETWORK, "MP match: this game start is a match");
			resetWatch();			// (a second match in the same run of the app starts from scratch)
			resultsDone = false;
			lostPeer = false;
			clearPaused();
			matchDevices();
			return;
		}
		NOTICE_LOG(NETWORK, "MP: a match start that did not happen, netplay switched off");
		active = false;
		clearNetplay();
		endLine = "The match could not start";
		config::VrMpEndText.set("The match could not start");
		config::VrMpEndText.save();
		saveBool(config::VrMpReturn, true);
	}
	else if (marker < 0 && !active)
	{
		NOTICE_LOG(NETWORK, "MP: a match was left running, netplay switched off");
		clearNetplay();
		saveBool(config::VrMpReturn, true);
	}
}

bool takeReturn()
{
	if (!config::VrMpReturn)
		return false;
	saveBool(config::VrMpReturn, false);
	return true;
}

// ---- automatic match setup (task 3.4) ----
//
// The game's own menus, driven by pad presses that go through the netplay (no memory writes but one: the
// arenas, below). Addresses: tcvr/docs/RE/Addresses.md ("Multiplayer menus").

namespace
{

constexpr u32 MenuRequest = 0x8C0A5A6C;		// menu page request bits
constexpr u32 SetupPageBit = 0x4000;		// team and vehicle page
constexpr u32 OptionsPageBit = 0x10000;		// multiplayer options page
constexpr u32 OneTeamBit = 0x10000000;		// "Only one team has been selected... continue?" (A yes, B no)
constexpr u32 Slots = 0x8C0A6048;			// 4 x 0x28: +0 team, +4 vehicle, +8 state (0 none, 1 pad, 2 choosing, 3 ready)
constexpr u32 OptRoom = 0x8C0A5AD0;
constexpr u32 OptMode = 0x8C0A5ACC;
constexpr u32 OptLimit = 0x8C0A60E8;		// the limit line's index 0..21, live while the page is open
constexpr u32 OptLine = 0x8C0A5A78;			// the options page's cursor: 0 Room, 1 Mode, 2 Active Environment, 3 limit
constexpr u32 RoomsMask = 0x8C0A5A5C;		// rooms unlocked (bit r-1 = room r, bit 7 extras)
constexpr u32 ResultsPageBit = 0x80000000;	// the end-of-match results page (0x8C02FAD8)
constexpr u32 TeamScores = 0x8C0C7DD4;		// 4 x u32, by team
constexpr u32 KillTable = 0x8C0CDC68;		// 4 rows (killer) x 0x14: 4 x u32 by victim, then the team points
constexpr int Fps = 60;
constexpr int Hold = 9;						// frames a button is held
constexpr int Gap = 12;						// frames between two presses

u32 read32(u32 a) { return addrspace::read32(a); }
bool onPage(u32 bit) { return (read32(MenuRequest) & bit) != 0; }
int slotState(int player) { return (int)read32(Slots + player * 0x28 + 8); }

// The route from the start of the game (a blank memory card, as every copy has in a match) to the main
// menu, in frames: measured with the PC netplay test (tcvr/tools/netplay-test-tc.ps1). Start skips the
// logos, the film and the title, picks the card and "New Game"; on the name entry Start types an A, Up
// then Right x4 reach END and A confirms; on the main menu Down is Multiplayer and A opens its first page.
struct Timed { int at; u32 buttons; };
const Timed bootRoute[] = {
	{ 3 * Fps, DC_BTN_START }, { 7 * Fps, DC_BTN_START }, { 11 * Fps, DC_BTN_START }, { 15 * Fps, DC_BTN_START },
	{ 19 * Fps, DC_BTN_START }, { 24 * Fps, DC_BTN_START }, { 25 * Fps + 30, DC_DPAD_UP },
	{ 26 * Fps + 30, DC_DPAD_RIGHT }, { 27 * Fps + 30, DC_DPAD_RIGHT }, { 28 * Fps + 30, DC_DPAD_RIGHT },
	{ 29 * Fps + 30, DC_DPAD_RIGHT }, { 31 * Fps, DC_BTN_A }, { 38 * Fps, DC_DPAD_DOWN }, { 40 * Fps, DC_BTN_A },
};

enum class Step { Off, Boot, WaitSetup, Ready, WaitOptions, Options, Back, WaitBack,
	GuestWait, GuestJoin, Done, Failed };

struct Setup
{
	bool started = false;
	int frame = 0;			// frames of the match so far
	Step step = Step::Off;
	int stepFrame = 0;		// when the step began
	int tries = 0;
	int bootIndex = 0;
	u32 holding = 0;
	int holdLeft = 0;
	int gapLeft = 0;
	int setupSeen = -1;		// guests: the first frame on the team page
	bool optionsSeen = false;
	bool optionsLeft = false;
	int mode = 0, room = 0, points = 10;
	int limitIndex = lobby::DefaultLimitIndex;
} su;

void go(Step s)
{
	su.step = s;
	su.stepFrame = su.frame;
	su.tries = 0;
}

void tap(u32 buttons)
{
	su.holding = buttons;
	su.holdLeft = Hold;
	su.gapLeft = Gap;
}

void fail(const char *why)
{
	WARN_LOG(NETWORK, "MP SETUP failed: %s (frame %d)", why, su.frame);
	go(Step::Failed);
}

int since() { return su.frame - su.stepFrame; }

// The end of the match (task 3.5): the game's results page opens, the scores are kept, and when the page
// closes (a player pressed on), or after a minute of it, every copy goes back to the lobby. Read once per
// new frame (never in a rollback), the same on every copy.
struct ResultsWatch
{
	int seenFrames = 0;		// frames in a row with the page open
	int goneFrames = 0;		// frames in a row with the page closed, after it was open
	bool open = false;
	int openFrame = 0;
	bool reported = false;
} rw;

Results readResults()
{
	Results r;
	const int n = std::clamp((int)matchNames.size(), 0, 4);
	for (int p = 0; p < n; p++)
	{
		Results::Line l;
		l.name = matchNames[p];
		l.team = (int)read32(Slots + p * 0x28) & 3;
		l.points = (int)read32(TeamScores + l.team * 4);
		for (int v = 0; v < 4; v++)
			if (v != p)
				l.kills += (int)read32(KillTable + p * 0x14 + v * 4);
		r.lines.push_back(l);
	}
	std::stable_sort(r.lines.begin(), r.lines.end(), [](const Results::Line& a, const Results::Line& b) {
		return a.points != b.points ? a.points > b.points : a.kills > b.kills;
	});
	r.valid = n > 0;
	return r;
}

void watchResults(int frame)
{
	const bool page = onPage(ResultsPageBit);
	if (!rw.open)
	{
		rw.seenFrames = page ? rw.seenFrames + 1 : 0;
		if (rw.seenFrames >= 6)
		{
			rw.open = true;
			rw.openFrame = frame;
			rw.goneFrames = 0;
			const Results r = readResults();
			{
				std::lock_guard<std::mutex> lock(resultsMutex);
				lastResults = r;
			}
			NOTICE_LOG(NETWORK, "MP match: the results page is open (frame %d)", frame);
			for (const Results::Line& l : r.lines)
				NOTICE_LOG(NETWORK, "MP results: %s team %d, %d points, %d kills", l.name.c_str(), l.team + 1, l.points, l.kills);
		}
		return;
	}
	if (page)
	{
		rw.goneFrames = 0;
		const Results r = readResults();	// (the page may still count up)
		std::lock_guard<std::mutex> lock(resultsMutex);
		lastResults = r;
	}
	else
		rw.goneFrames++;
	if (!rw.reported && (rw.goneFrames >= 30 || frame - rw.openFrame > 60 * Fps))
	{
		rw.reported = true;
		NOTICE_LOG(NETWORK, "MP match: the results page %s (frame %d): back to the lobby", rw.goneFrames >= 30 ? "closed" : "stayed a minute", frame);
		resultsDone = true;
	}
}

}	// namespace

static void resetWatch()
{
	su = Setup();
	rw = ResultsWatch();
}

uint32_t setupButtons()
{
	if (!inMatch())
		return 0;
	Setup& s = su;
	if (!s.started)
	{
		s.started = true;
		unsigned m = 0, r = 0, p = 10;
		sscanf(config::VrMpSetup.get().c_str(), "%u,%u,%u", &m, &r, &p);
		s.mode = (int)std::min(m, 2u);
		s.room = (int)std::min(r, 7u);
		s.points = (int)p;
		s.limitIndex = lobby::limitIndex(s.mode, s.points);
		go(ggpo::localPlayerIndex() == 0 ? Step::Boot : Step::GuestWait);
		NOTICE_LOG(NETWORK, "MP SETUP: player %d, mode %d room %d points %d", ggpo::localPlayerIndex() + 1, s.mode, s.room, s.points);
	}
	s.frame++;
	watchResults(s.frame);
	if (s.holdLeft > 0)
	{
		s.holdLeft--;
		return s.holding;
	}
	if (s.gapLeft > 0)
	{
		s.gapLeft--;
		return 0;
	}
	switch (s.step)
	{
	case Step::Boot:
		if (s.bootIndex < (int)(sizeof(bootRoute) / sizeof(bootRoute[0])))
		{
			if (s.frame >= bootRoute[s.bootIndex].at)
				tap(bootRoute[s.bootIndex++].buttons);
		}
		else
			go(Step::WaitSetup);
		break;
	case Step::WaitSetup:
		if (onPage(SetupPageBit))
			go(Step::Ready);
		else if (since() > 15 * Fps)
			fail("the team and vehicle page did not open");
		break;
	case Step::Ready:
		// player 1 alone ready (the others have not joined yet): the game goes to its options page
		if (since() > Fps / 2)
		{
			tap(DC_BTN_A);
			go(Step::WaitOptions);
		}
		break;
	case Step::WaitOptions:
		if (onPage(OneTeamBit) && !onPage(OptionsPageBit))
		{
			// player 1 alone: the game asks whether to go on with one team; yes
			if (since() > Fps / 3)
			{
				tap(DC_BTN_A);
				s.stepFrame = s.frame;
			}
		}
		else if (onPage(OptionsPageBit))
		{
			NOTICE_LOG(NETWORK, "MP SETUP: options page (room %d mode %d limit index %d)", read32(OptRoom), read32(OptMode), read32(OptLimit));
			go(Step::Options);
		}
		else if (since() > 2 * Fps)
		{
			if (++s.tries > 3)
				fail("the options page did not open");
			else
			{
				tap(DC_BTN_A);
				s.stepFrame = s.frame;
			}
		}
		break;
	case Step::Options:
	{
		// one line at a time, top to bottom, each checked in memory: Room, Mode, then the limit
		const int line = (int)read32(OptLine);
		const int room = (int)read32(OptRoom), mode = (int)read32(OptMode), limit = (int)read32(OptLimit);
		int want = -1;			// the line to change, -1 all done
		if (room != s.room)
			want = 0;
		else if (mode != s.mode)
			want = 1;
		else if (limit != s.limitIndex)
			want = 3;
		if (want < 0)
		{
			NOTICE_LOG(NETWORK, "MP SETUP: options entered: room %d mode %d limit index %d", room, mode, limit);
			go(Step::Back);
		}
		else if (++s.tries > 60)
			fail("the options did not take the match's settings");
		else if (line != want)
			tap(line < want ? DC_DPAD_DOWN : DC_DPAD_UP);
		else if (want == 3)
		{
			// the shorter way round (the index wraps both ways)
			const int ahead = (s.limitIndex - limit + lobby::LimitCount) % lobby::LimitCount;
			tap(ahead <= lobby::LimitCount / 2 ? DC_DPAD_RIGHT : DC_DPAD_LEFT);
		}
		else
			tap(DC_DPAD_RIGHT);
		break;
	}
	case Step::Back:
		tap(DC_BTN_B);
		go(Step::WaitBack);
		break;
	case Step::WaitBack:
		if (onPage(SetupPageBit) && !onPage(OptionsPageBit))
		{
			NOTICE_LOG(NETWORK, "MP SETUP done: team and vehicle page, room %d mode %d limit index %d (frame %d)", read32(OptRoom), read32(OptMode), read32(OptLimit), s.frame);
			go(Step::Done);
		}
		else if (since() > 2 * Fps)
		{
			if (++s.tries > 3)
				fail("no way back from the options page");
			else
			{
				tap(DC_BTN_B);
				s.stepFrame = s.frame;
			}
		}
		break;
	case Step::GuestWait:
		// join the team page once player 1 has been to the options page and is back (or after a while)
		if (onPage(OptionsPageBit))
			s.optionsSeen = true;
		if (onPage(SetupPageBit))
		{
			if (s.setupSeen < 0)
				s.setupSeen = s.frame;
			if ((s.optionsSeen && !onPage(OptionsPageBit)) || s.frame - s.setupSeen > 20 * Fps)
				go(Step::GuestJoin);
		}
		else if (s.frame > 90 * Fps)
			fail("the team and vehicle page did not open");
		break;
	case Step::GuestJoin:
		if (slotState(ggpo::localPlayerIndex()) >= 2)
		{
			NOTICE_LOG(NETWORK, "MP SETUP done: player %d on the team and vehicle page (frame %d)", ggpo::localPlayerIndex() + 1, s.frame);
			go(Step::Done);
		}
		else if (s.tries++ > 4)
			fail("could not join the team and vehicle page");
		else
		{
			tap(DC_BTN_START);
			s.gapLeft = Fps / 2;
		}
		break;
	default:
		break;
	}
	return s.holdLeft > 0 ? (s.holdLeft--, s.holding) : 0;
}

namespace
{

// All eight arenas, the same on every copy: under netplay the memory card is blank, so the game has only
// the rooms a new game opens. Every frame of a match (in rollbacks too, at the same point of the frame on
// every copy), once a game is loaded (mask not 0), every room is open.
void openAllRooms(Event, void *)
{
	if (!inMatch())
		return;
	const u8 mask = addrspace::read8(RoomsMask);
	if (mask != 0 && mask != 0xFF)
		addrspace::write8(RoomsMask, 0xFF);
}

struct RoomsRegistration
{
	RoomsRegistration() { EventManager::listen(Event::VBlank, openAllRooms); }
} roomsRegistration;

}

std::vector<std::string> optionsFor(const lobby::StartInfo& info, const std::string& hostAddress)
{
	return {
		"network:GGPO=yes",
		"network:GGPOPeers='" + info.peerList(hostAddress) + "'",
		"network:GGPOPlayer=" + std::to_string(info.localIndex),
		"network:GGPODelay=" + std::to_string(info.inputDelay),
	};
}

// ---- test commands over the pad listener's UDP port ("MP <command>", core/input/udp_lightgun.cpp) ----

namespace
{

const char *phaseName(lobby::Phase p)
{
	switch (p)
	{
	case lobby::Phase::Idle: return "idle";
	case lobby::Phase::Connecting: return "connecting";
	case lobby::Phase::InLobby: return "lobby";
	case lobby::Phase::Started: return "started";
	case lobby::Phase::Refused: return "refused";
	case lobby::Phase::HostLeft: return "hostleft";
	case lobby::Phase::Failed: return "failed";
	}
	return "?";
}

std::string stateLine()
{
	poll();
	const lobby::View v = theLobby().view();
	std::string line = std::string("MPR ") + phaseName(v.phase) + " players=" + std::to_string(v.state.count())
			+ " slot=" + std::to_string(v.mySlot) + " ready=";
	for (const lobby::PlayerInfo& p : v.state.players)
		line += p.present ? (p.ready ? '1' : '0') : '-';
	line += " mode=" + std::to_string(v.state.settings.mode) + " room=" + std::to_string(v.state.settings.room)
			+ " points=" + std::to_string(v.state.settings.points);
	line += " host=" + std::to_string(v.state.hostSlot) + (v.role == lobby::Role::Host ? " role=host" : v.role == lobby::Role::Joiner ? " role=joiner" : " role=none");
	line += std::string(" match=") + (inMatch() ? "1" : "0");
	if (v.phase == lobby::Phase::Started)
		line += " index=" + std::to_string(v.start.localIndex) + " of=" + std::to_string(v.start.peers.size());
	if (!v.message.empty())
		line += " msg=" + v.message;
	return line;
}

std::string udpCommand(const char *cmd)
{
	char name[16] = "";
	unsigned mode = 0, room = 0, points = 10, n = 0;
	char arg[64] = "";
	lobby::Lobby& l = theLobby();
	if (sscanf(cmd, "HOST %15s %u %u %u", name, &mode, &room, &points) >= 1 && strncmp(cmd, "HOST", 4) == 0)
	{
		lobby::Identity id = identity();
		id.name = name;
		std::string error;
		if (!l.host(id, { (uint8_t)mode, (uint8_t)room, (uint16_t)points }, error))
			return "MPR error " + error;
		return stateLine();
	}
	if (strncmp(cmd, "JOIN", 4) == 0)
	{
		sscanf(cmd, "JOIN %15s %63s", name, arg);
		l.startSearch();
		if (arg[0] != 0)
			l.query(arg);
		for (int i = 0; i < 40 && l.games().empty(); i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		const auto games = l.games();
		if (games.empty())
			return "MPR nogame";
		lobby::Identity id = identity();
		if (name[0] != 0)
			id.name = name;
		l.join(id, games.front().address, games.front().beacon.lobbyPort);
		l.stopSearch();
		for (int i = 0; i < 40 && l.view().phase == lobby::Phase::Connecting; i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		return stateLine();
	}
	if (sscanf(cmd, "READY %u", &n) == 1)
	{
		l.setReady(n != 0);
		return stateLine();
	}
	if (strncmp(cmd, "START", 5) == 0)
	{
		n = 2;
		sscanf(cmd, "START %u", &n);
		std::string error;
		if (!l.start((uint8_t)n, error))
			return "MPR error " + error;
		return stateLine();
	}
	if (strncmp(cmd, "LEAVEMATCH", 10) == 0)
	{
		leaveMatch();
		return "MPR left";
	}
	if (strncmp(cmd, "LEAVE", 5) == 0)
	{
		l.leave();
		return stateLine();
	}
	if (strncmp(cmd, "STATE", 5) == 0)
		return stateLine();
	if (strncmp(cmd, "RETURN", 6) == 0)
		return takeReturn() ? "MPR return=1 why=" + endText() : "MPR return=0";
	return "MPR unknown";
}

struct Registration
{
	Registration() { udpCommandHook = udpCommand; }
} registration;

}

}
