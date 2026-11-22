/*
	tcvr: the Multiplayer page of the desktop menu (Toy Commander VR, PC client, step B1).

	The same lobby and match as the VR menu's Multiplayer section (rend/vr/xr_menu.cpp), for a PC on a
	monitor: name, Create game (mode, room, points), Join game (games found on the LAN, or an address),
	the lobby (players, ready, Start), the ranking after a match. Everything goes through tcvr::match and
	tcvr::lobby, so a PC and a headset meet in the same lobby; this file only draws it with ImGui.

	See docs/multiplayer/PC-crossplay-analysis.md.

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "gui.h"
#include "gui_util.h"
#include "imgui.h"
#include "IconsFontAwesome6.h"
#include "network/tcvr_match.h"
#include "network/tcvr_lobby.h"
#include "cfg/option.h"
#include "emulator.h"
#include "oslib/oslib.h"
#include <chrono>
#include <string>

namespace lobby = tcvr::lobby;
namespace match = tcvr::match;

namespace
{

enum class Page { Top, Create, Join, Lobby };

Page page = Page::Top;
lobby::Settings createSettings;
bool searching;
bool ready;
std::string status;
std::string addressText;
std::string nameText;
int64_t leaveAskedMs;
// the player has used this page: from then on the desktop drives the match loop (test copies driven by
// UDP commands never open it, so their behaviour stays as it was)
bool desktopUsed;

// the same input delay as the headsets (xr_menu.cpp MatchInputDelay): 2 frames
constexpr uint8_t MatchInputDelay = 2;

int64_t nowMs()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void stopSearch()
{
	if (searching)
	{
		match::theLobby().stopSearch();
		searching = false;
	}
}

void show(Page p)
{
	if (page == Page::Join && p != Page::Join)
		stopSearch();
	page = p;
	if (p == Page::Join && !searching)
	{
		match::theLobby().startSearch();
		searching = true;
		if (!addressText.empty())
			match::theLobby().query(addressText);
	}
}

bool lobbyActive()
{
	return match::lobbyStarted() && match::theLobby().view().phase != lobby::Phase::Idle;
}

void heading(const char *text)
{
	ImGui::PushFont(nullptr, uiLargeFontSize());
	ImGui::Text("%s", text);
	ImGui::PopFont();
	ImGui::Separator();
}

void close()
{
	stopSearch();
	gui_setState(GuiState::Commands);
}

void drawSettingsEditor(lobby::Settings& s)
{
	int mode = s.mode;
	if (ImGui::Combo("Mode", &mode, lobby::ModeNames, 3))
	{
		const int index = lobby::limitIndex(s.mode, s.points);
		s.mode = (uint8_t)mode;
		s.points = (uint16_t)lobby::limitValue(s.mode, index);
	}
	int room = s.room;
	if (ImGui::Combo("Room", &room, lobby::RoomNames, lobby::RoomCount))
		s.room = (uint8_t)room;
	int index = lobby::limitIndex(s.mode, s.points);
	const std::string limit = lobby::limitText(s.mode, s.points);
	if (ImGui::SliderInt(s.mode == 1 ? "Time limit" : "Points to win", &index, 0, lobby::LimitCount - 1, limit.c_str(),
			ImGuiSliderFlags_AlwaysClamp))
		s.points = (uint16_t)lobby::limitValue(s.mode, index);
}

void drawTop()
{
	heading(ICON_FA_USERS "  Multiplayer");
	if (nameText.empty())
		nameText = match::playerName();
	ImGui::SetNextItemWidth(uiScaled(120));
	if (InputText("Player name (3 letters)", &nameText, ImGuiInputTextFlags_CharsUppercase))
	{
		if (nameText.size() > 3)
			nameText.resize(3);
	}
	if (ImGui::IsItemDeactivatedAfterEdit() && !nameText.empty())
	{
		match::setPlayerName(nameText);
		nameText = match::playerName();
	}
	ImGui::Spacing();
	if (IconButton(ICON_FA_PLUS, "Create game", ScaledVec2(200, 50)).realize())
		show(Page::Create);
	ImGui::SameLine();
	if (IconButton(ICON_FA_MAGNIFYING_GLASS, "Join game", ScaledVec2(200, 50)).realize())
		show(Page::Join);
	ImGui::Spacing();
	if (isOpenGL(config::RendererType))
		ImGui::TextDisabled("Your own player's view fills the screen during a match.");
	else
	{
		ImGui::TextWrapped("The match shows only your own player's view with the OpenGL renderer. With this renderer you see the whole split screen.");
		if (ImGui::Button("Switch to OpenGL"))
		{
			config::RendererType = RenderType::OpenGL;
			SaveSettings();
			status = "OpenGL selected: restart the game for it to take effect";
		}
	}
}

void drawCreate()
{
	heading(ICON_FA_PLUS "  Create game");
	ImGui::PushItemWidth(uiScaled(300));
	drawSettingsEditor(createSettings);
	ImGui::PopItemWidth();
	ImGui::Spacing();
	if (IconButton(ICON_FA_PLAY, "Open the lobby", ScaledVec2(200, 50)).realize())
	{
		std::string error;
		if (match::theLobby().host(match::identity(), createSettings, error))
		{
			ready = false;
			status.clear();
			show(Page::Lobby);
		}
		else
			status = error;
	}
	ImGui::SameLine();
	if (ImGui::Button("Back", ScaledVec2(100, 50)))
		show(Page::Top);
}

void drawJoin()
{
	heading(ICON_FA_MAGNIFYING_GLASS "  Join game");
	const auto games = match::theLobby().games();
	if (games.empty())
		ImGui::TextDisabled("Searching for games on this network...");
	for (const lobby::FoundGame& g : games)
	{
		const lobby::Beacon& b = g.beacon;
		const std::string line = b.hostName + "   " + lobby::ModeNames[b.settings.mode % 3] + "   "
				+ lobby::RoomNames[b.settings.room % lobby::RoomCount] + "   " + std::to_string(b.players) + "/"
				+ std::to_string(b.maxPlayers) + (b.started ? "   (playing)" : "")
				+ "##" + std::to_string(g.hostId);
		DisabledScope _(b.started || b.players >= b.maxPlayers);
		if (ImGui::Button(line.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, uiScaled(40))))
		{
			stopSearch();
			ready = false;
			status.clear();
			match::theLobby().join(match::identity(), g.address, b.lobbyPort);
			show(Page::Lobby);
			return;
		}
	}
	ImGui::Spacing();
	ImGui::SetNextItemWidth(uiScaled(220));
	InputText("Host address", &addressText);
	ImGui::SameLine();
	if (ImGui::Button("Ask") && !addressText.empty())
	{
		match::theLobby().query(addressText);
		status = "Asking " + addressText + "...";
	}
	ImGui::SameLine();
	if (ImGui::Button("Refresh"))
	{
		stopSearch();
		show(Page::Join);
		status = "Searching...";
	}
	ImGui::Spacing();
	if (ImGui::Button("Back", ScaledVec2(100, 50)))
		show(Page::Top);
}

void drawResults()
{
	const match::Results r = match::results();
	if (!r.valid || r.lines.empty())
		return;
	ImGui::Spacing();
	ImGui::Text("Last match");
	if (ImGui::BeginTable("results", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
	{
		ImGui::TableSetupColumn("#");
		ImGui::TableSetupColumn("Player");
		ImGui::TableSetupColumn("Points");
		ImGui::TableSetupColumn("Kills");
		ImGui::TableHeadersRow();
		int rank = 1;
		for (const auto& l : r.lines)
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::Text("%d", rank++);
			ImGui::TableNextColumn(); ImGui::Text("%s", l.name.c_str());
			ImGui::TableNextColumn(); ImGui::Text("%d", l.points);
			ImGui::TableNextColumn(); ImGui::Text("%d", l.kills);
		}
		ImGui::EndTable();
	}
}

void drawLobby()
{
	lobby::Lobby& l = match::theLobby();
	const lobby::View v = l.view();
	heading(v.role == lobby::Role::Host ? ICON_FA_USERS "  Your game" : ICON_FA_USERS "  Lobby");
	switch (v.phase)
	{
	case lobby::Phase::Connecting:
		ImGui::Text("Joining the game...");
		break;
	case lobby::Phase::Started:
		ImGui::Text("Starting the match...");
		break;
	case lobby::Phase::Refused:
	case lobby::Phase::HostLeft:
	case lobby::Phase::Failed:
		ImGui::TextWrapped("%s", v.phase == lobby::Phase::Refused ? lobby::refuseText(v.refuse) : v.message.c_str());
		break;
	case lobby::Phase::Idle:
		show(Page::Top);
		return;
	case lobby::Phase::InLobby:
	{
		const lobby::State& s = v.state;
		ImGui::Text("Host %s   %s   %s   %s", s.players[s.hostSlot % lobby::MaxPlayers].name.c_str(),
				lobby::ModeNames[s.settings.mode % 3], lobby::RoomNames[s.settings.room % lobby::RoomCount],
				lobby::limitText(s.settings.mode, s.settings.points).c_str());
		if (v.role == lobby::Role::Host && !v.hostAddress.empty())
			ImGui::TextDisabled("Others can join by address: %s", v.hostAddress.c_str());
		ImGui::Spacing();
		for (int i = 0; i < lobby::MaxPlayers; i++)
		{
			const lobby::PlayerInfo& p = s.players[i];
			if (!p.present)
				ImGui::TextDisabled("P%d   (free)", i + 1);
			else
				ImGui::Text("P%d   %s%s%s", i + 1, p.name.c_str(), i == (int)s.hostSlot ? "   host" : p.ready ? "   ready" : "   not ready",
						i == v.mySlot ? "   (you)" : "");
		}
		ImGui::Spacing();
		if (v.role == lobby::Role::Host)
		{
			lobby::Settings edited = s.settings;
			ImGui::PushItemWidth(uiScaled(300));
			drawSettingsEditor(edited);
			ImGui::PopItemWidth();
			if (edited != s.settings)
				l.setSettings(edited);
			std::string why;
			const bool can = l.canStart(why);
			{
				DisabledScope _(!can);
				if (IconButton(ICON_FA_PLAY, "Start the match", ScaledVec2(200, 50)).realize() && can)
				{
					std::string error;
					status = l.start(MatchInputDelay, error) ? "Starting the match..." : error;
				}
			}
			if (!can)
				ImGui::TextDisabled("%s", why.c_str());
		}
		else
		{
			if (ImGui::Checkbox("Ready", &ready))
				l.setReady(ready);
		}
		drawResults();
		break;
	}
	}
	ImGui::Spacing();
	if (ImGui::Button("Leave the lobby", ScaledVec2(200, 40)))
	{
		l.leave();
		ready = false;
		status.clear();
		show(Page::Top);
	}
}

}	// namespace

void gui_tcvr_multiplayer()
{
	desktopUsed = true;
	fullScreenWindow(false);
	ImguiStyleVar _(ImGuiStyleVar_WindowBorderSize, 0);
	ImGui::Begin("##tcvrmp", nullptr, ImGuiWindowFlags_NoDecoration);
	{
		ImguiStyleVar _1(ImGuiStyleVar_FramePadding, ScaledVec2(10.f, 6.f));
		ImGui::Indent(uiScaled(10));
		if (ImGui::Button(ICON_FA_ARROW_LEFT "  Back to the game menu"))
			close();
		ImGui::Spacing();
		if (page != Page::Lobby && lobbyActive())
			show(Page::Lobby);
		switch (page)
		{
		case Page::Top: drawTop(); break;
		case Page::Create: drawCreate(); break;
		case Page::Join: drawJoin(); break;
		case Page::Lobby: drawLobby(); break;
		}
		if (!status.empty())
		{
			ImGui::Spacing();
			ImGui::TextWrapped("%s", status.c_str());
		}
		ImGui::Unindent(uiScaled(10));
	}
	ImGui::End();
}

// The desktop's share of the match loop (the VR menu does this itself while a headset is on): called once
// per frame by mainui. Starts and ends matches, and opens the Multiplayer page back on the lobby after one.
void gui_tcvr_poll(bool vrMenuDrives)
{
	if (vrMenuDrives || !desktopUsed)
		return;
	if (!match::lobbyStarted() && !match::inMatch())
		return;
	match::poll();
	if (emu.running() && match::takeReturn())
	{
		const std::string why = match::lastEnd();
		status = why.empty() ? "The match is over" : why;
		ready = false;
		page = Page::Lobby;
		try {
			emu.stop();
		} catch (...) {
		}
		gui_setState(GuiState::Multiplayer);
	}
	int id = 0;
	static int lastId;
	const std::string notice = match::notice(id);
	if (!notice.empty() && id != lastId)
	{
		lastId = id;
		os_notify(notice.c_str(), 4000);
	}
}

// The game menu key during a match: the emulator can't stop for a menu (the others' game goes on), so a
// first press says how to leave and a second one within 3 s leaves the match. True when it was handled.
bool gui_tcvr_menu_key()
{
	if (!desktopUsed || !match::inMatch())
		return false;
	const int64_t now = nowMs();
	if (now - leaveAskedMs < 3000)
	{
		leaveAskedMs = 0;
		status = match::leaveMatch();
		os_notify(status.c_str(), 3000);
	}
	else
	{
		leaveAskedMs = now;
		os_notify("Press the menu key again to leave the match", 3000);
	}
	return true;
}
