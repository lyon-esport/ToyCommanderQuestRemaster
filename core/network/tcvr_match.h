/*
	tcvr: the multiplayer match, from the lobby to the game (Toy Commander VR, multiplayer tasks 3.2/3.3).

	Glue between the lobby (tcvr_lobby.h) and the emulator's netplay (ggpo.cpp):

	- one Lobby for the whole process;
	- who this copy is (name, app version, disc, netplay port);
	- START: the options the netplay needs (network:GGPOPeers / GGPOPlayer / GGPO ...) go into emu.cfg together
	  with a marker (vr.MpMarker) and the game is started again, which the platform does (restartHook: on the
	  headset the app launches itself again, the way "Import saves" does);
	- at the next game load, gate(): a start that carries a fresh marker is a match; anything else that
	  looks like a left-over match (the app was closed during one, a start that failed) is switched off, so a
	  later single-player game never finds itself in netplay;
	- leaving a match switches netplay off and starts the game again.

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#pragma once
#include "tcvr_lobby.h"
#include <string>

namespace tcvr::match
{

// The platform's "start the game again" (returns a line for the menu). Null: not available (PC: logs
// "MP RESTART", and the test script restarts the copy).
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

// Call often (once per headset frame is fine): starts the match when the lobby says START.
void poll();
// True while this run of the game is a match.
bool inMatch();
// The player left the match from the VR menu (or wants out): netplay off, game started again.
std::string leaveMatch();
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
