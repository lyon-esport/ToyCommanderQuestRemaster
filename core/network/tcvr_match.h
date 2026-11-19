/*
	tcvr: the multiplayer match, from the lobby to the game (Toy Commander VR, multiplayer tasks 3.2/3.3).

	Glue between the lobby (tcvr_lobby.h) and the emulator's netplay (ggpo.cpp):

	- one Lobby for the whole process;
	- who this copy is (name, app version, disc, netplay port);
	- START: the options the netplay needs (network:GGPOPeers / GGPOPlayer / GGPO ...) go into emu.cfg together
	  with a marker (vr.MpMarker) and the game is loaded again inside the running app (reloadNow, done by the
	  render thread in mainui.cpp; the app itself is not restarted any more: on some headsets it came back
	  behind the Quest's own screens);
	- at the next game load, gate(): a start that carries a fresh marker is a match; anything else that
	  looks like a left-over match (the app was closed during one, a start that failed) is switched off, so a
	  later single-player game never finds itself in netplay;
	- the end of a match (the game's results page closed, a player left, Leave match, an error): netplay off,
	  the lobby (still connected through the match) back on its page with the same players, the game loaded
	  again in single player, the VR menu opened on the lobby with the ranking.

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#pragma once
#include "tcvr_lobby.h"
#include <string>
#include <vector>

namespace tcvr::match
{

// The platform's "start the app again" (returns a line for the menu). No longer used by matches (they load
// the game again inside the running app); kept for the platform glue.
extern std::string (*restartHook)();
// The platform's way to read the player name of the first saved game ("" if none).
extern std::string (*saveNameHook)();

lobby::Lobby& theLobby();
// True once the lobby has been made (it is made on first use: the Multiplayer pages).
bool lobbyStarted();
lobby::Identity identity();

// The player's name: 3 letters kept in emu.cfg, by default the first saved game's name.
std::string playerName();
void setPlayerName(const std::string& name);

// Call often (once per headset frame is fine): starts the match when the lobby says START, ends it when a
// player left or the game's results page has been shown.
void poll();
// True while this run of the game is a match.
bool inMatch();
// A player left or the network dropped (GGPO gave up on a peer): the next poll() ends the match.
void peerLost();
// GGPO: a player's copy went silent (paused: headset off, the app in the background) or came back.
void peerInterrupted(int player, bool interrupted);
// The player left the match from the VR menu (or wants out): netplay off, the game loaded again.
std::string leaveMatch();

// Matches start and end inside the running app: the game is loaded again with the netplay options on
// (or off). True once when the render thread should do that now (gui_start_game, mainui.cpp).
bool reloadNow();
// The game stopped with an error (gui_stop_game): in a match, or while one starts, this ends the match,
// and true says the caller must load the game again in single player (not show an error nobody sees in
// the headset). False: not a match, the caller goes on as usual.
bool gameStopped(const std::string& message);
// True when a match start has waited too long for the other players (the caller cancels the netplay).
bool startTimedOut();
// What the headset shows instead of the game: a match starting, ending, or a paused player. False: nothing.
bool panelText(std::string& line, std::string& detail);

// The last match's results page, read from the game (task 3.5): best first.
struct Results
{
	struct Line { std::string name; int team = 0; int points = 0; int kills = 0; };
	std::vector<Line> lines;
	bool valid = false;
};
Results results();
// Why the last match ended ("" if none yet this run).
std::string lastEnd();

// Called at each game load, before the netplay starts (emulator.cpp).
void gate();
// True once after a match has ended: the menu should open on its Multiplayer page.
bool takeReturn();

// Automatic match setup (task 3.4): the buttons this copy's own pad adds to its netplay input for the
// frame about to be sent (Dreamcast button bits, pressed = 1). Called once per new frame (never in a
// rollback) by the netplay, only in a match. Player 1's copy goes through the game's menus to the team
// and vehicle page and enters the match's Mode, Room and Points on the options page; the other copies
// join the page with Start. Everything goes through the netplay as pad input, so every copy runs the
// same game; the copies only read memory to know where the game is.
uint32_t setupButtons();

// The options a START puts in emu.cfg, as text "key=value" lines, for the tests.
std::vector<std::string> optionsFor(const lobby::StartInfo& info, const std::string& hostAddress);

}
