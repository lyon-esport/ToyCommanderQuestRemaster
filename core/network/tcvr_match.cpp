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
#include "version.h"

#include <algorithm>
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
}

std::string restart()
{
	if (restartHook != nullptr)
		return restartHook();
	// PC (the test script restarts the copy when it sees this line)
	WARN_LOG(NETWORK, "MP RESTART");		// (a warning: always in the log)
	return "Restart needed";
}

std::string upper(const std::string& s)
{
	std::string out;
	for (char c : s)
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ')
			out += (char)std::toupper((unsigned char)c);
	return out;
}

void startMatch(const lobby::View& v)
{
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
	saveInt(config::VrMpMarker, nowSeconds() + MarkerSeconds);
	restart();
}

}	// namespace

lobby::Lobby& theLobby()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (!theOne)
	{
		theOne = std::make_unique<lobby::Lobby>();
		theOne->setBroadcastHook([](bool on) { enableNetworkBroadcast(on); });
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

void poll()
{
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

std::string leaveMatch()
{
	active = false;
	{
		std::lock_guard<std::mutex> lock(mutex);
		begun = false;
	}
	clearNetplay();
	saveBool(config::VrMpReturn, true);
	return restart();
}

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
			return;
		}
		NOTICE_LOG(NETWORK, "MP: a match start that did not happen, netplay switched off");
		active = false;
		clearNetplay();
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
		return takeReturn() ? "MPR return=1" : "MPR return=0";
	return "MPR unknown";
}

struct Registration
{
	Registration() { udpCommandHook = udpCommand; }
} registration;

}

}
