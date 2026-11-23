/*
	tcvr: the VR menu. See xr_menu.h.

	Grouped in sections (Manu, 2026-11-16): the top page has Resume, Recenter view, one line per
	section (Display, Image quality, Performance, Comfort, Saves, Multiplayer) and Quit game; A opens a
	section, "Back" or B returns to the top page, B on the top page closes the menu. A section without
	any line in this build isn't shown.

	Multiplayer (tasks 3.2 and 3.3): Create game, Join game and Player name; the lobby page once a game
	is created or joined (tcvr_lobby.h, tcvr_match.h). In a match the menu doesn't freeze the game: it
	offers Resume, Recenter, Display/Image/Performance/Comfort (the settings that must be the same on
	every copy are locked), Leave match and Quit.

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "xr_menu.h"
#include "xr_panel.h"
#include "xr_mp_pages.h"
#include "cfg/option.h"
#include "emulator.h"
#include "hw/pvr/Renderer_if.h"
#include "rend/CustomTexture.h"
#include "log/Log.h"
#include "network/tcvr_lobby.h"
#include "network/tcvr_match.h"
#include "network/tcvr_arena_pics.h"
#include "input/gamepad.h"
#include "stdclass.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

namespace vr::xr
{

std::string (*menuRequestHook)(MenuRequest request);

namespace
{

using Clock = std::chrono::steady_clock;

enum class Section { Display, Image, Performance, Comfort, Saves, Multiplayer, Count };
const char *const SectionNames[] = { "Display", "Image quality", "Performance", "Comfort", "Saves", "Multiplayer" };

// The Multiplayer section's own pages.
enum class MpPage { Top, Create, Join, Lobby };
using tcvr::lobby::ModeNames;
using tcvr::lobby::RoomCount;

bool open;
bool pausedGame;			// the menu stopped the emulator (and starts it again)
float placedYaw;			// where the head looked when it opened
float lastHeadYaw;			// where the head looked at the last update
float noticeYaw;			// tcvr: where the head looked when the match notice came ("BBB left the match")
int noticeSeen = 0;
int page = -1;				// -1: the top page, else a Section
int selected;
std::string status;			// the last action's result
Clock::time_point statusUntil;
int confirming = -1;		// the action waiting for a second A (import, quit)

MpPage mpPage = MpPage::Top;
tcvr::lobby::Settings mpSettings;	// the Create game page's choices
std::string mpAddress;				// "Enter address" on the Join page
bool mpReady;						// this copy's Ready in the lobby
bool mpSearching;					// the Join page listens

// A short text edited with the stick: up/down changes the letter, left/right moves, A keeps it, B drops it.
struct TextEdit
{
	bool active = false;
	int action = -1;			// the Action it was started on
	std::string text;
	int cursor = 0;
	const char *charset = "";	// the last one stands for "nothing" (a space)
	size_t maxLen = 3;
};
TextEdit edit;

// stick navigation, with key-repeat
int heldDir;				// 0 none, 1 up, 2 down, 3 left, 4 right
Clock::time_point nextRepeat;

// frame rates, over the last second
u32 headsetFrames;
u32 lastFrameCount;
Clock::time_point rateSince;
float headsetFps, gameFps;

// A setting with fixed steps, shown by name, in a section.
struct Choice
{
	Section section;
	const char *label;
	std::vector<float> values;
	std::vector<const char *> names;
	std::function<float()> get;
	std::function<void(float)> set;
	bool matchLocked = false;	// the same on every copy in a match: not changeable there (it shows how it is forced)
};

void pauseGame();
void resumeGame();
void setStatus(const std::string& s);

void saveOption(config::Option<float>& o, float v) { o.set(v); o.save(); }
void saveOption(config::Option<int>& o, float v) { o.set((int)std::lround(v)); o.save(); }

const std::vector<Choice>& choices()
{
	static const std::vector<Choice> list {
		// supersampling: more pixels per eye than the headset's recommended size, sharper
		{ Section::Display, "Resolution multiplier", { 1.f, 1.1f, 1.2f, 1.3f, 1.4f, 1.5f, 1.6f }, { "100%", "110%", "120%", "130%", "140%", "150%", "160%" },
			[] { return (float)config::VrXrResolution; }, [](float v) { saveOption(config::VrXrResolution, v); } },
		{ Section::Image, "Anti-aliasing (MSAA)", { 1.f, 2.f, 4.f }, { "Off", "2x", "4x" },
			[] { return (float)config::VrMsaa; }, [](float v) { saveOption(config::VrMsaa, v); } },
		{ Section::Image, "Anisotropic filtering", { 1.f, 2.f, 4.f, 8.f, 16.f }, { "Off", "2x", "4x", "8x", "16x" },
			[] { return (float)config::AnisotropicFiltering; }, [](float v) { saveOption(config::AnisotropicFiltering, v); } },
		{ Section::Image, "Sharper far textures", { 0.f, -0.5f, -1.f }, { "Off", "Low (-0.5)", "High (-1.0)" },
			[] { return (float)config::VrTexLodBias; }, [](float v) { saveOption(config::VrTexLodBias, v); } },
		// the upscale cache (CustomTexture.cpp): each texture upscaled once, then reused
		// (each size has its own cache; a change applies at the next start)
		{ Section::Image, "Textures", { 0.f, 2.f, 4.f }, { "Original", "HD 2x", "HD 4x" },
			[] { return (float)config::VrUpscaleCache; }, [](float v) {
				saveOption(config::VrUpscaleCache, v);
				setStatus("Textures: applies at the next start of the game");
			} },
		// the headset's display rate (applied live by xr_host); the game itself stays at 60 Hz
		{ Section::Performance, "Refresh rate", { 72.f, 90.f, 120.f }, { "72 Hz", "90 Hz", "120 Hz" },
			[] { return (float)config::VrXrRefreshRate; }, [](float v) { saveOption(config::VrXrRefreshRate, v); } },
		// tcvr: the game's own picture rate in missions (spg.cpp): 60/s instead of the stock 30/s
		{ Section::Performance, "Game pictures in missions", { 0.f, 1.f }, { "30/s (original)", "60/s" },
			[] { return config::VrGame60 ? 1.f : 0.f; }, [](float v) { config::VrGame60.set(v > 0.5f); config::VrGame60.save(); tcvrApplyGameRate(true); }, true },
		// the frame-rate counter at the bottom right of the view
		{ Section::Performance, "Show frame rate", { 0.f, 1.f }, { "Off", "On" },
			[] { return config::VrShowFps ? 1.f : 0.f; }, [](float v) { config::VrShowFps.set(v > 0.5f); config::VrShowFps.save(); } },
		// tcvr: fixed foveated rendering: fewer pixels at the lens edges, more frame rate
		{ Section::Performance, "Fixed foveation (edges)", { 0.f, 1.f }, { "Off", "On" },
			[] { return (float)config::VrFoveation; }, [](float v) { saveOption(config::VrFoveation, v); } },
		// tcvr: the other frame-rate tweaks, each switchable to compare (both on by default)
		{ Section::Performance, "Upload frame once", { 0.f, 1.f }, { "Off", "On" },
			[] { return config::VrUploadOnce ? 1.f : 0.f; }, [](float v) { config::VrUploadOnce.set(v > 0.5f); config::VrUploadOnce.save(); } },
		{ Section::Performance, "Discard eye depth", { 0.f, 1.f }, { "Off", "On" },
			[] { return config::VrDiscardDepth ? 1.f : 0.f; }, [](float v) { config::VrDiscardDepth.set(v > 0.5f); config::VrDiscardDepth.save(); } },
		{ Section::Comfort, "Comfort vignette", { 0.f, 1.f, 2.f, 3.f }, { "Off", "Low", "Medium", "High" },
			[] { return (float)config::VrComfortVignette; }, [](float v) { saveOption(config::VrComfortVignette, v); } },
		// tcvr: the head-driven camera (xr_headcam.cpp): look all around the vehicle
		{ Section::Comfort, "Head turns the camera", { 0.f, 1.f, 2.f }, { "Off", "Left/right", "Left/right + up/down" },
			[] { return (float)config::VrHeadCamera; }, [](float v) { saveOption(config::VrHeadCamera, v); }, true },
	};
	return list;
}

// A line of the page shown.
enum class Action { Resume, Recenter, OpenSection, Back, Choice, WorldSize, Export, Import, UnlockAll, Quit,
	// Multiplayer
	MpCreate, MpJoin, MpName, MpMode, MpRoom, MpPoints, MpStartHosting, MpGame, MpRefresh, MpAddress,
	MpReady, MpStartMatch, MpLeave, LeaveMatch, Info };
struct Entry
{
	Action action;
	int index = -1;		// the choice (Choice) or the section (OpenSection)
	uint32_t id = 0;	// MpGame: the host's id
	std::string text;	// Info, MpGame
};

bool inMatch() { return tcvr::match::inMatch(); }
bool lobbyActive()
{
	if (!tcvr::match::lobbyStarted())
		return false;
	const auto phase = tcvr::match::theLobby().view().phase;
	return phase != tcvr::lobby::Phase::Idle;
}

Entry infoLine(const std::string& text)
{
	Entry e { Action::Info };
	e.text = text;
	return e;
}

bool isInfo(const Entry& e) { return e.action == Action::Info; }

constexpr uint8_t MatchInputDelay = 2;
std::string roomName(int room) { return tcvr::lobby::RoomNames[room % RoomCount]; }

// The pages of the Multiplayer section: what can be picked on them (they are drawn as cards and
// buttons, xr_mp_pages.h; B goes back, so they have no Back line).
std::vector<Entry> mpEntries()
{
	using namespace tcvr;
	std::vector<Entry> list;
	switch (mpPage)
	{
	case MpPage::Top:
		list.push_back({ Action::MpCreate });
		list.push_back({ Action::MpJoin });
		list.push_back({ Action::MpName });
		break;
	case MpPage::Create:
		if (arena::busy())
		{
			list.push_back({ Action::Back });		// (the arena pictures are being made: Back stops it)
			break;
		}
		list.push_back({ Action::MpMode });
		list.push_back({ Action::MpRoom });
		list.push_back({ Action::MpPoints });
		list.push_back({ Action::MpStartHosting });
		break;
	case MpPage::Join:
		for (const auto& g : match::theLobby().games())
		{
			Entry e { Action::MpGame };
			e.id = g.hostId;
			list.push_back(e);
		}
		list.push_back({ Action::MpAddress });
		list.push_back({ Action::MpRefresh });
		break;
	case MpPage::Lobby:
	{
		const lobby::View v = match::theLobby().view();
		if (v.phase == lobby::Phase::InLobby)
			list.push_back({ v.role == lobby::Role::Host ? Action::MpStartMatch : Action::MpReady });
		if (v.phase == lobby::Phase::InLobby || v.phase == lobby::Phase::Connecting)
			list.push_back({ Action::MpLeave });
		else if (v.phase == lobby::Phase::Started)
			list.push_back(infoLine("Starting the match..."));
		else
			list.push_back({ Action::Back });		// refused, host left, failed: the page says it
		break;
	}
	}
	return list;
}

std::vector<Entry> entries()
{
	std::vector<Entry> list;
	const auto& all = choices();
	const bool match = inMatch();
	if (page < 0)
	{
		list.push_back({ Action::Resume });
		list.push_back({ Action::Recenter });
		for (int s = 0; s < (int)Section::Count; s++)
		{
			const Section sec = (Section)s;
			// (saving, importing and the multiplayer pages start the game again: not in a match)
			if (match && (sec == Section::Saves || sec == Section::Multiplayer))
				continue;
			bool any = sec == Section::Display || sec == Section::Saves || sec == Section::Multiplayer;
			for (const Choice& c : all)
				any = any || c.section == sec;
			if (any)
				list.push_back({ Action::OpenSection, s });
		}
		if (match)
			list.push_back({ Action::LeaveMatch });
		list.push_back({ Action::Quit });
		return list;
	}
	if ((Section)page == Section::Multiplayer)
		return mpEntries();
	list.push_back({ Action::Back });
	if ((Section)page == Section::Display)
		list.push_back({ Action::WorldSize });
	for (int i = 0; i < (int)all.size(); i++)
		if (all[i].section == (Section)page)
			list.push_back({ Action::Choice, i });
	if ((Section)page == Section::Saves)
	{
		list.push_back({ Action::Export });
		list.push_back({ Action::Import });
		// Toy Commander (PAL) only: the save layout is known for that disc alone
		if (settings.content.gameId == "MK-5102050")
			list.push_back({ Action::UnlockAll });
	}
	return list;
}

// The first line to pick on a page: after Back (and after the information lines).
int firstSelectable(const std::vector<Entry>& list)
{
	for (int i = 0; i < (int)list.size(); i++)
		if (!isInfo(list[i]) && !(i == 0 && list[i].action == Action::Back && list.size() > 1))
			return i;
	return 0;
}

int nearestIndex(const Choice& c)
{
	const float v = c.get();
	int best = 0;
	for (size_t i = 1; i < c.values.size(); i++)
		if (std::abs(c.values[i] - v) < std::abs(c.values[best] - v))
			best = (int)i;
	return best;
}

void setStatus(const std::string& s)
{
	status = s;
	statusUntil = Clock::now() + std::chrono::seconds(6);
	NOTICE_LOG(RENDERER, "XR menu: %s", s.c_str());
}

void pauseGame()
{
	if (pausedGame || !emu.running())
		return;
	try {
		emu.stop();
		pausedGame = true;
	} catch (const FlycastException& e) {
		WARN_LOG(RENDERER, "XR menu: couldn't pause the game: %s", e.what());
	}
}

void resumeGame()
{
	if (!pausedGame)
		return;
	pausedGame = false;
	try {
		emu.start();
	} catch (const FlycastException& e) {
		ERROR_LOG(RENDERER, "XR menu: couldn't resume the game: %s", e.what());
	}
}

void mpStopSearch()
{
	if (!mpSearching)
		return;
	mpSearching = false;
	tcvr::match::theLobby().stopSearch();
}

// A page of the Multiplayer section.
void mpShow(MpPage p)
{
	if (mpPage == MpPage::Join && p != MpPage::Join)
		mpStopSearch();
	mpPage = p;
	confirming = -1;
	edit.active = false;
	// the arena pictures, made once from the player's own disc (a short tour of the game's room select page)
	if (p == MpPage::Create)
		tcvr::arena::startIfMissing();
	if (p == MpPage::Join)
	{
		auto& l = tcvr::match::theLobby();
		l.startSearch();
		mpSearching = true;
		if (!mpAddress.empty())
			l.query(mpAddress);
	}
	selected = firstSelectable(entries());
}

void openMenu(float headYaw)
{
	open = true;
	placedYaw = headYaw;
	page = -1;
	selected = 0;
	confirming = -1;
	heldDir = 0;
	edit.active = false;
	status.clear();
	// always frozen behind the menu (Manu): the game must not go on without the player. Except in a
	// multiplayer match: the other players' game goes on (the vehicle stands idle), and so does this one.
	if (!inMatch())
		pauseGame();
	NOTICE_LOG(RENDERER, "XR menu: open (game %s)", pausedGame ? "paused" : inMatch() ? "in a match, running" : "not running");
}

void showPage(int newPage)
{
	const int from = page;
	page = newPage;
	confirming = -1;
	selected = 0;
	edit.active = false;
	if (from == (int)Section::Multiplayer && newPage != from)
		mpStopSearch();
	if (newPage < 0)
	{
		// back on the top page: on the section left
		const auto list = entries();
		for (int i = 0; i < (int)list.size(); i++)
			if (list[i].action == Action::OpenSection && list[i].index == from)
				selected = i;
	}
	else if (newPage == (int)Section::Multiplayer)
	{
		// a game already created or joined: its lobby
		mpPage = lobbyActive() ? MpPage::Lobby : MpPage::Top;
		if (mpPage == MpPage::Join)
			mpStopSearch();
		selected = firstSelectable(entries());
	}
	else
		selected = 1;	// the section's first setting, below Back
	if (newPage >= 0)
		NOTICE_LOG(RENDERER, "XR menu: section %s", SectionNames[newPage]);
}

// One step back: out of a Multiplayer page, or out of the section.
void goBack()
{
	if (page == (int)Section::Multiplayer && mpPage != MpPage::Top)
	{
		// back on the start page: on the tile the player came from (Join for the games found or a
		// joined game's lobby, else Host)
		Action tile = Action::MpCreate;
		if (mpPage == MpPage::Join
				|| (mpPage == MpPage::Lobby && tcvr::match::theLobby().view().role != tcvr::lobby::Role::Host))
			tile = Action::MpJoin;
		if (mpPage == MpPage::Create)
			tcvr::arena::cancel();		// (the arena pictures stop: the player's game comes back)
		if (mpPage == MpPage::Lobby)
		{
			tcvr::match::theLobby().leave();
			mpReady = false;
		}
		mpShow(MpPage::Top);
		const auto list = entries();
		for (int i = 0; i < (int)list.size(); i++)
			if (list[i].action == tile)
				selected = i;
		return;
	}
	showPage(-1);
}

std::string request(MenuRequest r)
{
	if (menuRequestHook == nullptr)
		return "Not available on this platform";
	return menuRequestHook(r);
}

// What the Multiplayer code needs of the platform: start the game again, the first saved game's name.
struct MpHooks
{
	MpHooks()
	{
		// (no platform hook, as on the PC: the match code logs "MP RESTART" for the test script)
		tcvr::match::restartHook = [] () -> std::string { return menuRequestHook == nullptr ? std::string() : request(MenuRequest::Restart); };
		tcvr::match::saveNameHook = [] () -> std::string { return menuRequestHook == nullptr ? std::string() : request(MenuRequest::SaveName); };
	}
} mpHooks;

bool isMatchLocked(const Entry& e)
{
	return e.action == Action::Choice && inMatch() && choices()[e.index].matchLocked;
}

// the limit line's step, as the game's options page counts it (one index for every mode)
int pointsIndex()
{
	return tcvr::lobby::limitIndex(mpSettings.mode, mpSettings.points);
}

// Mode, Room and Points to win: step (stick) or go round (A).
void mpChange(Action a, int step, bool wrap)
{
	auto move = [&](int value, int count) {
		int v = value + step;
		if (wrap)
			v = (v + count) % count;
		return std::clamp(v, 0, count - 1);
	};
	if (a == Action::MpMode)
	{
		// the game keeps the limit's step when the mode changes (points <-> time)
		const int index = pointsIndex();
		mpSettings.mode = (uint8_t)move(mpSettings.mode, 3);
		mpSettings.points = (uint16_t)tcvr::lobby::limitValue(mpSettings.mode, index);
	}
	else if (a == Action::MpRoom)
		mpSettings.room = (uint8_t)move(mpSettings.room, RoomCount);
	else if (a == Action::MpPoints)
		mpSettings.points = (uint16_t)tcvr::lobby::limitValue(mpSettings.mode, move(pointsIndex(), tcvr::lobby::LimitCount));
}

void change(const Entry& e, int step)
{
	if (e.action == Action::WorldSize)
	{
		const float scale = std::clamp(config::VrWorldScale * (step > 0 ? 1.1f : 1.f / 1.1f), 0.005f, 0.2f);
		saveOption(config::VrWorldScale, scale);
		return;
	}
	if (e.action == Action::MpMode || e.action == Action::MpRoom || e.action == Action::MpPoints)
	{
		mpChange(e.action, step, false);
		return;
	}
	if (e.action == Action::MpReady)
	{
		const bool ready = step > 0;
		if (ready && tcvr::match::backFromMatch())
		{
			setStatus("Wait a moment: the game is coming back from the last match");
			return;
		}
		if (ready != mpReady)
		{
			mpReady = ready;
			tcvr::match::theLobby().setReady(mpReady);
		}
		return;
	}
	if (e.action == Action::Choice)
	{
		if (isMatchLocked(e))
			return;
		const Choice& c = choices()[e.index];
		const int i = std::clamp(nearestIndex(c) + step, 0, (int)c.values.size() - 1);
		c.set(c.values[i]);
		NOTICE_LOG(RENDERER, "XR menu: %s = %s", c.label, c.names[i]);
	}
}

// Text editing (player name, address).
void beginEdit(Action a)
{
	edit = TextEdit();
	edit.active = true;
	edit.action = (int)a;
	if (a == Action::MpName)
	{
		edit.text = tcvr::match::playerName();
		edit.charset = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
		edit.maxLen = 3;
		edit.cursor = 0;
	}
	else
	{
		edit.text = mpAddress;
		edit.charset = "0123456789. ";
		edit.maxLen = 15;
		edit.cursor = (int)std::min(edit.text.size(), edit.maxLen - 1);
	}
	status.clear();		// (the footer shows the keys)
}

std::string editDisplay()
{
	std::string s;
	const int n = (int)std::max<size_t>(edit.text.size(), edit.cursor + 1);
	for (int i = 0; i < n; i++)
	{
		char c = i < (int)edit.text.size() ? edit.text[i] : ' ';
		if (c == ' ')
			c = '_';
		if (i == edit.cursor)
			s += std::string("[") + c + "]";
		else
			s += c;
	}
	return s;
}

void editStep(int dir)
{
	const std::string cs = edit.charset;
	if (dir == 3)
		edit.cursor = std::max(0, edit.cursor - 1);
	else if (dir == 4)
	{
		if (edit.cursor + 1 < (int)edit.maxLen)
			edit.cursor++;
	}
	else
	{
		while ((int)edit.text.size() <= edit.cursor)
			edit.text.push_back(' ');
		const size_t at = cs.find(edit.text[edit.cursor]);
		const int count = (int)cs.size();
		int i = at == std::string::npos ? 0 : (int)at;
		i = (i + (dir == 1 ? 1 : count - 1)) % count;
		edit.text[edit.cursor] = cs[i];
	}
}

void commitEdit()
{
	std::string t = edit.text;
	while (!t.empty() && t.back() == ' ')
		t.pop_back();
	edit.active = false;
	if (edit.action == (int)Action::MpName)
	{
		if (t.empty())
			return;
		tcvr::match::setPlayerName(t);
		setStatus("Player name: " + tcvr::match::playerName());
	}
	else
	{
		mpAddress = t;
		if (t.empty())
			return;
		tcvr::match::theLobby().query(t);
		setStatus("Asking " + t + " ...");
	}
}

MenuAction accept(const Entry& e)
{
	using namespace tcvr;
	switch (e.action)
	{
	case Action::Resume:
		closeMenu();
		return MenuAction::None;
	case Action::Recenter:
		closeMenu();
		return MenuAction::Recenter;
	case Action::OpenSection:
		showPage(e.index);
		return MenuAction::None;
	case Action::Back:
		goBack();
		return MenuAction::None;
	case Action::Choice:
		if (isMatchLocked(e))
		{
			setStatus("Fixed during a match: the same on every copy");
			return MenuAction::None;
		}
		change(e, 1);
		return MenuAction::None;
	case Action::WorldSize:
		change(e, 1);
		return MenuAction::None;
	case Action::Export:
		setStatus(request(MenuRequest::ExportSaves));
		return MenuAction::None;
	case Action::Import:
	case Action::UnlockAll:
	case Action::Quit:
	case Action::LeaveMatch:
	{
		// A twice
		if (confirming != (int)e.action)
		{
			confirming = (int)e.action;
			setStatus(e.action == Action::Quit ? "Press A again to quit"
					: e.action == Action::LeaveMatch ? "Press A again: you leave the match, back to the lobby"
					: e.action == Action::Import ? "Press A again: replaces your saves and restarts the game"
					: "Press A again: opens every mission of your saved games (backed up first) and restarts the game");
			return MenuAction::None;
		}
		confirming = -1;
		if (e.action == Action::LeaveMatch)
			setStatus(match::leaveMatch());
		else
			setStatus(request(e.action == Action::Quit ? MenuRequest::Quit
					: e.action == Action::Import ? MenuRequest::ImportSaves : MenuRequest::UnlockAll));
		return MenuAction::None;
	}
	case Action::MpCreate:
		mpShow(MpPage::Create);
		return MenuAction::None;
	case Action::MpJoin:
		mpShow(MpPage::Join);
		return MenuAction::None;
	case Action::MpName:
	case Action::MpAddress:
		beginEdit(e.action);
		return MenuAction::None;
	case Action::MpMode:
	case Action::MpRoom:
	case Action::MpPoints:
		mpChange(e.action, 1, true);
		return MenuAction::None;
	case Action::MpStartHosting:
	{
		std::string error;
		if (match::theLobby().host(match::identity(), mpSettings, error))
		{
			mpReady = false;
			mpShow(MpPage::Lobby);
		}
		else
			setStatus(error);
		return MenuAction::None;
	}
	case Action::MpGame:
	{
		for (const auto& g : match::theLobby().games())
			if (g.hostId == e.id)
			{
				mpStopSearch();
				mpReady = false;
				match::theLobby().join(match::identity(), g.address, g.beacon.lobbyPort);
				mpShow(MpPage::Lobby);
				return MenuAction::None;
			}
		setStatus("That game is gone");
		return MenuAction::None;
	}
	case Action::MpRefresh:
	{
		auto& l = match::theLobby();
		l.stopSearch();
		l.startSearch();
		mpSearching = true;
		if (!mpAddress.empty())
			l.query(mpAddress);
		setStatus("Searching...");
		return MenuAction::None;
	}
	case Action::MpReady:
		change(e, mpReady ? -1 : 1);
		return MenuAction::None;
	case Action::MpStartMatch:
	{
		std::string error;
		// 2 frames of input delay (33 ms at 60 Hz): fewer rollbacks on Wi-Fi, which the headsets pay for
		if (!match::theLobby().start(MatchInputDelay, error))
			setStatus(error);
		else
			setStatus("Starting the match...");
		return MenuAction::None;
	}
	case Action::MpLeave:
		goBack();
		return MenuAction::None;
	case Action::Info:
		return MenuAction::None;
	}
	return MenuAction::None;
}

bool hasValue(const Entry& e)
{
	if (isMatchLocked(e))
		return false;
	return e.action == Action::Choice || e.action == Action::WorldSize || e.action == Action::MpMode
			|| e.action == Action::MpRoom || e.action == Action::MpPoints;
}

std::string lineText(const Entry& e)
{
	char buf[160];
	switch (e.action)
	{
	case Action::Resume: return "Resume";
	case Action::Recenter: return "Recenter view";
	case Action::OpenSection: return std::string(SectionNames[e.index]) + "  ...";
	case Action::Back: return "Back";
	case Action::WorldSize:
		snprintf(buf, sizeof(buf), "World size:  %d%%", (int)std::lround(config::VrWorldScale / 0.11f * 100.f));
		return buf;
	case Action::Choice:
	{
		const Choice& c = choices()[e.index];
		snprintf(buf, sizeof(buf), "%s:  %s%s", c.label, c.names[nearestIndex(c)], isMatchLocked(e) ? "  (fixed in a match)" : "");
		return buf;
	}
	case Action::Export: return "Export saves to Download";
	case Action::Import: return "Import saves from Download";
	case Action::UnlockAll: return "Unlock all levels";
	case Action::Quit: return "Quit game";
	case Action::LeaveMatch: return "Leave match";
	case Action::MpCreate: return "Create game  ...";
	case Action::MpJoin: return "Join game  ...";
	case Action::MpName:
		return "Player name:  " + (edit.active && edit.action == (int)Action::MpName ? editDisplay() : tcvr::match::playerName());
	case Action::MpMode: return std::string("Mode:  ") + ModeNames[mpSettings.mode % 3];
	case Action::MpRoom: return "Room:  " + roomName(mpSettings.room % RoomCount);
	case Action::MpPoints: return std::string(mpSettings.mode == 1 ? "Time limit:  " : "Points to win:  ") + tcvr::lobby::limitText(mpSettings.mode, mpSettings.points);
	case Action::MpStartHosting: return "Start hosting";
	case Action::MpGame: return e.text;
	case Action::MpRefresh: return "Refresh";
	case Action::MpAddress:
		if (edit.active && edit.action == (int)Action::MpAddress)
			return "Address:  " + editDisplay();
		return "Address:  " + (mpAddress.empty() ? std::string("enter one") : mpAddress);
	case Action::MpReady: return mpReady ? "Ready:  Yes" : "Ready:  No";
	case Action::MpStartMatch:
	{
		std::string why;
		const bool can = tcvr::match::theLobby().canStart(why);
		return can ? "Start match" : "Start match   (" + why + ")";
	}
	case Action::MpLeave: return "Leave game";
	case Action::Info: return e.text;
	}
	return "";
}

// ---- the Multiplayer pages as cards and buttons (Manu chose layout A, 2026-11-21) ------------

// the limit's value alone ("10", "8 min", "No limit"), for the host page's button
std::string limitValueText(int mode, int points)
{
	std::string t = tcvr::lobby::limitText(mode, points);
	for (const char *tail : { " points to win", " point to win", " time limit" })
	{
		const size_t at = t.find(tail);
		if (at != std::string::npos)
			return t.substr(0, at);
	}
	return t == "no limit" ? "No limit" : t;
}

std::string summaryText(const tcvr::lobby::Settings& st)
{
	return std::string(ModeNames[st.mode % 3]) + "  \xC2\xB7  " + roomName(st.room % RoomCount) + "  \xC2\xB7  "
			+ tcvr::lobby::limitText(st.mode, st.points);
}

mp::Item mpItem(Action a)
{
	switch (a)
	{
	case Action::MpCreate: return mp::Item::Host;
	case Action::MpJoin: return mp::Item::Join;
	case Action::MpName: return mp::Item::Name;
	case Action::MpMode: return mp::Item::Mode;
	case Action::MpRoom: return mp::Item::Room;
	case Action::MpPoints: return mp::Item::Points;
	case Action::MpStartHosting: return mp::Item::StartHosting;
	case Action::MpGame: return mp::Item::Game;
	case Action::MpAddress: return mp::Item::Address;
	case Action::MpRefresh: return mp::Item::Refresh;
	case Action::MpReady: return mp::Item::Ready;
	case Action::MpStartMatch: return mp::Item::StartMatch;
	case Action::MpLeave: return mp::Item::Leave;
	case Action::Back: return mp::Item::Back;
	default: return mp::Item::None;
	}
}

// What the Multiplayer page shows now, for xr_mp_pages.
mp::Screen mpScreen(const std::vector<Entry>& list)
{
	using namespace tcvr;
	mp::Screen sc;
	for (const Entry& e : list)
		sc.items.push_back(mpItem(e.action));
	sc.selected = selected;
	const std::string sep = " \xE2\x80\xBA ";
	const bool editName = edit.active && edit.action == (int)Action::MpName;
	const bool editAddress = edit.active && edit.action == (int)Action::MpAddress;
	sc.editing = edit.active;
	switch (mpPage)
	{
	case MpPage::Top:
		sc.page = mp::Page::Top;
		sc.crumb = "VR menu" + sep + "Multiplayer";
		sc.name = editName ? editDisplay() : match::playerName();
		sc.nameEditing = editName;
		break;
	case MpPage::Create:
		sc.crumb = "Multiplayer" + sep + "Host a game";
		if (arena::busy())
		{
			sc.page = mp::Page::Message;
			sc.message = arena::progressText() + "\nOnce only, about 20 seconds: your game comes back after.";
			break;
		}
		sc.page = mp::Page::Create;
		if (const auto pic = arena::picture(mpSettings.room % RoomCount))
		{
			sc.picture = pic->rgba.data();
			sc.pictureW = pic->width;
			sc.pictureH = pic->height;
			sc.pictureId = pic->id;
			sc.pictureKeep = pic;
		}
		sc.mode = ModeNames[mpSettings.mode % 3];
		sc.room = roomName(mpSettings.room % RoomCount);
		sc.roomIndex = mpSettings.room % RoomCount;
		sc.roomCount = RoomCount;
		sc.limitLabel = mpSettings.mode == 1 ? "Time limit" : "Points to win";
		sc.limit = limitValueText(mpSettings.mode, mpSettings.points);
		break;
	case MpPage::Join:
	{
		sc.page = mp::Page::Join;
		sc.crumb = "Multiplayer" + sep + "Join a game";
		const auto games = match::theLobby().games();
		// (in the order of the list's game lines)
		for (const Entry& e : list)
			if (e.action == Action::MpGame)
				for (const auto& g : games)
					if (g.hostId == e.id)
					{
						const lobby::Beacon& b = g.beacon;
						mp::GameCard c;
						c.host = b.hostName;
						c.mode = ModeNames[b.settings.mode % 3];
						c.room = roomName(b.settings.room % RoomCount);
						c.limit = lobby::limitText(b.settings.mode, b.settings.points);
						c.players = b.players;
						c.maxPlayers = b.maxPlayers;
						c.started = b.started;
						sc.games.push_back(c);
						break;
					}
		sc.address = editAddress ? editDisplay() : mpAddress.empty() ? std::string("Press A to type one") : mpAddress;
		sc.addressEditing = editAddress;
		break;
	}
	case MpPage::Lobby:
	{
		sc.crumb = "Multiplayer" + sep + "Lobby";
		const lobby::View v = match::theLobby().view();
		if (v.phase != lobby::Phase::InLobby)
		{
			sc.page = mp::Page::Message;
			sc.message = v.phase == lobby::Phase::Connecting ? (v.message.empty() ? "Connecting to the game..." : v.message)
					: v.phase == lobby::Phase::Started ? "Starting the match..."
					: v.message.empty() ? "The game is closed" : v.message;
			break;
		}
		sc.page = mp::Page::Lobby;
		const lobby::State& st = v.state;
		sc.isHost = v.role == lobby::Role::Host;
		sc.summary = summaryText(st.settings);
		for (int i = 0; i < lobby::MaxPlayers; i++)
		{
			const lobby::PlayerInfo& p = st.players[i];
			mp::Slot sl;
			sl.present = p.present;
			sl.name = p.name;
			sl.you = i == v.mySlot;
			sl.host = i == st.hostSlot;
			sl.ready = p.ready;
			sc.slots.push_back(sl);
		}
		sc.hostAddress = v.hostAddress;
		std::string why;
		sc.canStart = sc.isHost && match::theLobby().canStart(why);
		sc.whyNot = why;
		sc.ready = mpReady;
		const match::Results r = match::results();
		if (r.valid)
			for (const auto& l : r.lines)
				sc.results.push_back({ l.name, l.points, l.kills });
		sc.message = v.message;
		break;
	}
	}
	return sc;
}

// The Multiplayer page drawn; the place of each line of list.
std::vector<mp::Rect> mpLayout(const std::vector<Entry>& list, Panel* panel = nullptr)
{
	Panel scratch;
	mp::Screen sc = mpScreen(list);
	const bool showStatus = !status.empty() && Clock::now() < statusUntil;
	if (showStatus)
		sc.status = status;
	char rates[96];
	snprintf(rates, sizeof(rates), "Headset %.0f fps   Game %.0f fps%s", headsetFps, gameFps, pausedGame ? " (paused)" : "");
	sc.rates = rates;
	return mp::build(sc, panel != nullptr ? *panel : scratch);
}

}	// namespace

bool menuOpen()
{
	return open;
}

void frameRates(float& headset, float& game)
{
	headset = headsetFps;
	game = gameFps;
}

void forgetPausedGame()
{
	if (pausedGame)
		NOTICE_LOG(RENDERER, "XR menu: the game is loaded again: the menu won't start it");
	pausedGame = false;
}

void closeMenu()
{
	if (!open)
		return;
	open = false;
	confirming = -1;
	page = -1;
	edit.active = false;
	mpStopSearch();
	tcvr::arena::cancel();		// (arena pictures being made: they stop, the player's game comes back and runs)
	resumeGame();
	NOTICE_LOG(RENDERER, "XR menu: closed");
}

void noteHeadsetFrame()
{
	headsetFrames++;
	const auto now = Clock::now();
	if (rateSince == Clock::time_point())
	{
		rateSince = now;
		lastFrameCount = FrameCount;
		return;
	}
	const float secs = std::chrono::duration<float>(now - rateSince).count();
	if (secs >= 1.f)
	{
		headsetFps = headsetFrames / secs;
		// (the game loaded again in the running app, for a match: its frame count starts again from 0;
		// the unsigned difference showed "game 4271623" for a second)
		gameFps = FrameCount >= lastFrameCount ? (FrameCount - lastFrameCount) / secs : 0.f;
		headsetFrames = 0;
		lastFrameCount = FrameCount;
		rateSince = now;
	}
}

bool sameEntry(const Entry& a, const Entry& b)
{
	return a.action == b.action && a.index == b.index && a.id == b.id;
}

// The Multiplayer pages change by themselves (a player joins, a game appears, the lobby connects):
// the cursor stays on the line it was on, goes to the first useful line when the lobby's phase
// changes or the first game appears, and never rests on an information line.
void keepSelection(const std::vector<Entry>& list)
{
	static Entry last { Action::Info };
	static int lastIndex = -1, lastPage = -2, lastPhase = -1, lastGames = 0;
	const int pageKey = page == (int)Section::Multiplayer ? 100 + (int)mpPage : page;
	int phase = -1;
	if (page == (int)Section::Multiplayer && mpPage == MpPage::Lobby && tcvr::match::lobbyStarted())
		phase = (int)tcvr::match::theLobby().view().phase;
	const int games = (int)std::count_if(list.begin(), list.end(), [](const Entry& e) { return e.action == Action::MpGame; });
	if (pageKey == lastPage && selected == lastIndex)
	{
		if (phase != lastPhase || (lastGames == 0 && games > 0))
			selected = firstSelectable(list);
		else if (!sameEntry(list[selected], last))
		{
			auto it = std::find_if(list.begin(), list.end(), [](const Entry& e) { return sameEntry(e, last); });
			selected = it != list.end() ? (int)(it - list.begin()) : firstSelectable(list);
		}
	}
	if (isInfo(list[selected]))
		selected = firstSelectable(list);
	last = list[selected];
	lastIndex = selected;
	lastPage = pageKey;
	lastPhase = phase;
	lastGames = games;
}

// The game's own Multiplayer (Manu, 2026-11-19): the Dreamcast's team and vehicle page waits for more pads
// plugged in, but in VR the players are other headsets, met in the VR menu's Multiplayer section. When that
// page opens in single player, the menu player leaves it with B (as on the Dreamcast: the "MASTER" quarter
// aborts the page, the game goes back to its main menu) and the VR menu opens on its Multiplayer section.
static struct GameMultiplayer
{
	enum class Step { Idle, Leaving, Left, Failed } step = Step::Idle;
	int seen = 0;			// headset frames in a row with the page open (a word read mid-frame can flicker)
	int tries = 0;
	std::chrono::steady_clock::time_point pressUntil, nextPress, goneSince;
	bool gone = false;
} gm;
static uint32_t gmButtons;

uint32_t gameButtons()
{
	return gmButtons;
}

static void redirectGameMultiplayer(float headYaw)
{
	using namespace std::chrono;
	gmButtons = 0;
	if (!emu.running())
		return;		// (paused, loading: the page is neither open nor gone)
	const auto now = steady_clock::now();
	const bool page = !open && !pausedGame && tcvr::match::gameSetupPageOpen();
	switch (gm.step)
	{
	case GameMultiplayer::Step::Idle:
		gm.seen = page ? gm.seen + 1 : 0;
		if (gm.seen >= 3)
		{
			NOTICE_LOG(RENDERER, "XR menu: the game's Multiplayer page opened: leaving it for the VR menu's");
			gm.step = GameMultiplayer::Step::Leaving;
			gm.tries = 0;
			gm.gone = false;
			gm.nextPress = now + milliseconds(100);	// (the menu player's quarter is ready for B)
		}
		break;
	case GameMultiplayer::Step::Leaving:
		if (open)
		{
			gm.step = GameMultiplayer::Step::Failed;	// the player opened the menu meanwhile
			break;
		}
		if (!page && !pausedGame)
		{
			if (!gm.gone)
			{
				gm.gone = true;
				gm.goneSince = now;
			}
			// the main menu back on screen behind the VR menu
			if (now - gm.goneSince >= milliseconds(300))
			{
				NOTICE_LOG(RENDERER, "XR menu: the game's Multiplayer page left (%d presses): VR menu, Multiplayer", gm.tries);
				gm.step = GameMultiplayer::Step::Left;
				openMenu(headYaw);
				showPage((int)Section::Multiplayer);
			}
			break;
		}
		gm.gone = false;
		if (now < gm.pressUntil)
			gmButtons = DC_BTN_B;
		else if (now >= gm.nextPress)
		{
			if (gm.tries >= 5)
			{
				WARN_LOG(RENDERER, "XR menu: the game's Multiplayer page did not close with B");
				gm.step = GameMultiplayer::Step::Failed;
				break;
			}
			gm.tries++;
			gm.pressUntil = now + milliseconds(120);
			gm.nextPress = now + milliseconds(500);
			gmButtons = DC_BTN_B;
		}
		break;
	case GameMultiplayer::Step::Left:
	case GameMultiplayer::Step::Failed:
		// once the page is gone (and the menu closed), a new choice of Multiplayer goes the same way
		if (!page && !open)
		{
			gm.step = GameMultiplayer::Step::Idle;
			gm.seen = 0;
		}
		break;
	}
}

MenuAction updateMenu(const MenuInput& input, float headYaw)
{
	lastHeadYaw = headYaw;
	// (also with the menu closed: START from the host arrives whenever it likes)
	tcvr::match::poll();
	// the arena pictures made, the player's own game back: frozen behind the menu again
	if (tcvr::arena::takeFinished() && open && !inMatch())
		pauseGame();
	// a match began (the game was loaded again with the netplay on): the menu that started it closes
	{
		static bool wasMatch;
		const bool now = inMatch();
		if (now && !wasMatch && open)
			closeMenu();
		wasMatch = now;
	}
	// a match ended (or didn't start): once the game runs again, the menu opens on the lobby page
	if (emu.running() && tcvr::match::takeReturn())
	{
		closeMenu();
		openMenu(headYaw);
		mpReady = false;
		showPage((int)Section::Multiplayer);
		const std::string why = tcvr::match::lastEnd();
		setStatus(why.empty() ? "The match is over" : why);
		return MenuAction::None;
	}
	redirectGameMultiplayer(headYaw);
	if (input.toggle)
	{
		if (open)
			closeMenu();
		else
			openMenu(headYaw);
		return MenuAction::None;
	}
	if (!open)
		return MenuAction::None;
	if (input.back)
	{
		// B: drops a text edit, goes out of a section (or a page of it), or the menu closes
		if (edit.active)
			edit.active = false;
		else if (page >= 0)
			goBack();
		else
			closeMenu();
		return MenuAction::None;
	}
	const auto list = entries();
	selected = std::clamp(selected, 0, (int)list.size() - 1);
	keepSelection(list);
	// the stick: one step, then repeats while held
	int dir = 0;
	const glm::vec2& s = input.stick;
	if (std::max(std::abs(s.x), std::abs(s.y)) > 0.6f)
		dir = std::abs(s.y) >= std::abs(s.x) ? (s.y > 0.f ? 1 : 2) : (s.x < 0.f ? 3 : 4);
	const auto now = Clock::now();
	bool step = false;
	if (dir != heldDir)
	{
		heldDir = dir;
		step = dir != 0;
		nextRepeat = now + std::chrono::milliseconds(400);
	}
	else if (dir != 0 && now >= nextRepeat)
	{
		step = true;
		nextRepeat = now + std::chrono::milliseconds(140);
	}
	if (edit.active)
	{
		if (step)
			editStep(dir);
		if (input.accept)
			commitEdit();
		return MenuAction::None;
	}
	if (step && page == (int)Section::Multiplayer)
	{
		// the cards and buttons: left/right changes a value, else the stick goes to the nearest button
		// that way
		if ((dir == 3 || dir == 4) && hasValue(list[selected]))
			change(list[selected], dir == 3 ? -1 : 1);
		else
		{
			const int to = mp::move(mpLayout(list), selected, dir);
			if (to >= 0)
				selected = to;
			else if (dir == 3 && mpPage == MpPage::Top)	// (only from the start page: elsewhere a stray push left the
				goBack();								// page, or the game; B goes back)
		}
		confirming = -1;
	}
	else if (step)
	{
		const int count = (int)list.size();
		if (dir == 1 || dir == 2)
		{
			// (the information lines of the lobby are skipped)
			for (int tries = 0; tries < count; tries++)
			{
				selected = (selected + (dir == 1 ? -1 : 1) + count) % count;
				if (!isInfo(list[selected]))
					break;
			}
			confirming = -1;
		}
		else if (hasValue(list[selected]))
			change(list[selected], dir == 3 ? -1 : 1);
		else if (dir == 4 && list[selected].action == Action::OpenSection)
			showPage(list[selected].index);		// right opens a section too
		else if (dir == 3 && page >= 0)
			goBack();							// left goes back
	}
	if (input.accept)
	{
		const auto now2 = entries();
		return accept(now2[std::clamp(selected, 0, (int)now2.size() - 1)]);
	}
	return MenuAction::None;
}

// tcvr: with the menu closed, a match notice ("BBB left the match") for a few seconds: one line in front
// of where the head looked when it came, a little below eye level so it hides little of the game
static bool noticePanel(Panel& panel, float& yaw)
{
	int id;
	const std::string text = tcvr::match::notice(id);
	if (text.empty())
		return false;
	if (id != noticeSeen)
	{
		noticeSeen = id;
		noticeYaw = lastHeadYaw;
	}
	panel = Panel();
	panel.imageWidth = 1024;
	panel.imageHeight = 160;
	panel.widthMetres = 0.8f;
	panel.lines.push_back({ text, 0.42f, glm::vec3(1.f, 0.85f, 0.3f) });
	yaw = noticeYaw;
	return true;
}

bool menuPanel(Panel& panel, float& yaw)
{
	if (!open)
		return noticePanel(panel, yaw);
	// tcvr: brighter than before (unpicked lines were 60% grey), for contrast
	const glm::vec3 red(0.96f, 0.24f, 0.18f), white(1.f), grey(0.66f), yellow(1.f, 0.85f, 0.3f), soft(0.86f, 0.86f, 0.9f);
	panel = Panel();
	// tcvr: a bigger, finer picture than the first-start panel's: 1.2 x 0.78 m at 1.6 m, the
	// letters about 1.3 degrees high (they were 0.85), 1536 pixels across (1.5x the display's
	// pixels there, mipmapped)
	panel.imageWidth = 1536;
	panel.imageHeight = 1000;
	panel.widthMetres = 1.2f;
	yaw = placedYaw;
	const auto list = entries();
	if (page == (int)Section::Multiplayer)
	{
		mpLayout(list, &panel);		// (cards and buttons, xr_mp_pages.cpp)
		return true;
	}
	panel.lines.push_back({ "TOY COMMANDER VR", 0.052f, red });
	panel.lines.push_back({ page < 0 ? (inMatch() ? "VR menu   (match running)" : "VR menu") : SectionNames[page], 0.04f, grey });
	const float lineSize = list.size() > 9 ? 0.039f : 0.046f;
	for (int i = 0; i < (int)list.size(); i++)
	{
		std::string s = lineText(list[i]);
		const bool sel = i == selected;
		const bool info = isInfo(list[i]);
		if (sel && !info)
		{
			s = hasValue(list[i]) ? "<   " + s + "   >" : ">  " + s + "  <";
			panel.highlight = (int)panel.lines.size();
		}
		panel.lines.push_back({ s, lineSize, sel && !info ? (confirming == (int)list[i].action ? yellow : white) : info ? grey : soft });
	}
	char rates[96];
	int upscaled, queued, cached;
	custom_texture.getUpscaleCacheProgress(upscaled, queued, cached);
	snprintf(rates, sizeof(rates), "Headset %.0f fps   Game %.0f fps%s   HD textures %d", headsetFps, gameFps,
			pausedGame ? " (paused)" : "", cached);
	const bool showStatus = !status.empty() && Clock::now() < statusUntil;
	panel.lines.push_back({ showStatus ? status : std::string(rates), 0.032f, showStatus ? yellow : grey });
	yaw = placedYaw;
	return true;
}

void drawMenu(const glm::mat4& viewProj)
{
	Panel panel;
	float yaw;
	if (!menuPanel(panel, yaw))
		return;
	// in front of where the head looked when it opened
	drawPanel(viewProj * glm::rotate(glm::mat4(1.f), yaw, glm::vec3(0.f, 1.f, 0.f)), panel);
}

}
