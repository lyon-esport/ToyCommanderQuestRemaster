/*
	Copyright 2021 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#pragma once
#include <future>
#include <string>

struct MapleInputState;

namespace ggpo
{

std::future<bool> startNetwork();
void startSession(int localPort, int localPlayerNum);
void stopSession();
void getInput(MapleInputState inputState[4]);
bool nextFrame();
#ifdef __EMSCRIPTEN__
// tcvr: the browser build (no threads): once per tick before the emulator runs; false = don't run it now
bool browserTick();
#endif
bool active();
// tcvr: this copy's player (0 = player 1) and how many play, in a netplay session (else 0 and 1)
int localPlayerIndex();
int playerCount();
// tcvr: the frame the session is at (the last state saved: the same on every copy, in rollbacks too), -1 none
int currentFrame();
void displayStats();
void endOfFrame();
void sendChatMessage(int playerNum, const std::string& msg);
void receiveChatMessages(void (*callback)(int playerNum, const std::string& msg));

static inline bool rollbacking() {
	extern bool inRollback;

	return inRollback;
}

}
