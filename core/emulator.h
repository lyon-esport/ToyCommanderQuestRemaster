/*
	Created on: Apr 20, 2020

	Copyright 2020 flyinghead

	This file is part of flycast.

    flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once
#include "types.h"
#include "stdclass.h"

#include <atomic>
#include <future>
#include <array>
#include <mutex>
#include <utility>
#include <vector>
#include <time.h>

void loadGameSpecificSettings();
#ifdef USE_OPENXR
// tcvr: the only disc the VR build runs: Toy Commander (Europe). Every address in its VR profile
// (core/rend/vr_reproject.cpp) is this version's, and multiplayer will need the same disc everywhere.
constexpr const char *TCVR_SUPPORTED_GAME_ID = "MK-5102050";
// Told when another disc is refused (the platform shows the player why), before the load fails.
extern void (*vrUnsupportedGameHook)(const std::string& gameId);
// tcvr: raise or restore the emulated SH-4 clock for vr.Game60 (running: the game already runs)
void tcvrApplyGameRate(bool running);
// tcvr: spg.cpp tells it when a mission falls back to 30 pictures/s (true) and when 60 is tried again
void tcvrGameRateFellBehind(bool behind);
#endif
void SaveSettings();

int flycast_init(int argc, char* argv[]);
void flycast_term();
void dc_exit();
void dc_savestate(int index = 0, const u8 *pngData = nullptr, u32 pngSize = 0);
// tcvr: the whole machine state as bytes (the emulator stopped, or between two frames), a savestate file made
// of such bytes, and one loaded (the multiplayer: a match that starts from a saved state, the player's own
// game kept during a match). The same file format as the savestates.
bool dc_serializeAll(std::vector<u8>& out);
bool dc_writeStateFile(const std::string& path, const std::vector<u8>& state);
bool dc_loadStateFile(const std::string& path);
void dc_loadstate(int index = 0);
time_t dc_getStateCreationDate(int index);
void dc_getStateScreenshot(int index, std::vector<u8>& pngData);
bool dc_savestateAllowed();

enum class Event {
	Start,
	Pause,
	Resume,
	Terminate,
	LoadState,
	VBlank,
	Network,
	DiskChange,
	LocaleChange,
	max = LocaleChange
};

class EventManager
{
public:
	using Callback = void (*)(Event, void *);

	static void listen(Event event, Callback callback, void *param = nullptr) {
		instance().registerEvent(event, callback, param);
	}

	static void unlisten(Event event, Callback callback, void *param = nullptr) {
		instance().unregisterEvent(event, callback, param);
	}

	static void event(Event event) {
		instance().broadcastEvent(event);
	}

private:
	EventManager() = default;
	static EventManager& instance() {
		static EventManager _instance;
		return _instance;
	}

	void registerEvent(Event event, Callback callback, void *param);
	void unregisterEvent(Event event, Callback callback, void *param);
	void broadcastEvent(Event event);

	std::array<std::vector<std::pair<Callback, void *>>, static_cast<size_t>(Event::max) + 1> callbacks;
};

struct LoadProgress
{
	void reset()
	{
		cancelled = false;
		label = nullptr;
		progress = 0.f;
	}
	std::atomic<bool> cancelled;
	std::atomic<const char *> label;
	std::atomic<float> progress;
};

class Sh4Executor;

class Emulator
{
public:
	/**
	 * Initialize the emulator. Does nothing if already initialized.
	 */
	void init();
	/**
	 * Terminate the emulator. After calling this method, the application must be restarted.
	 */
	void term();
	/**
	 * Load the specified game/media. Will call init() first.
	 * May throw if the game cannot be loaded.
	 * If a game is already loaded, or the emulator is in the error state, unloadGame() must be called first.
	 */
	void loadGame(const char *path, LoadProgress *progress = nullptr);
	/**
	 * Reset the emulator in order to load another game. After calling this method, only loadGame() and term() can be called.
	 * Does nothing if no game is loaded.
	 */
	void unloadGame();
	/**
	 * Run the emulator in the calling thread until a frame is rendered. A game must be loaded and start() must be called
	 * prior to calling this method.
	 */
	void run();
	/**
	 * Prepare the emulator to start executing. Starts the execution thread if in multi-thread mode.
	 * A game must be loaded successfully before calling this method.
	 */
	void start();
	/**
	 * Stops the execution thread if in multi-thread mode. The game can be restarted by calling start().
	 * Does nothing if the game isn't running.
	 */
	void stop();
	/**
	 * Execute a single instruction.
	 */
	void step();
	/**
	 * Execute instructions while PC is with range.
	 */
	void stepRange(u32 from, u32 to);
	/**
	 * Return whether the emulator is currently running.
	 */
	bool running() const {
		return state == Running;
	}
	/**
	 * Wait for the next frame and render it. If in single-thread mode, it will run the emulator until a frame is rendered.
	 */
	bool render();
	/**
	 * Set the network state.
	 */
	void setNetworkState(bool online);
	/**
	 * Called internally to reset the emulator.
	 */
	void requestReset();
	/**
	 * Called internally on vblank.
	 */
	void vblank();
	/**
	 * Restart the cpu iff the emu is still running
	 * Returns true if the cpu was started
	 */
	bool restartCpu();
	/*
	 * Load the machine state from the passed deserializer
	 */
	void loadstate(Deserializer& deser);
	/**
	 * Insert a new media in the GD-ROM drive.
	 */
	void insertGdrom(const std::string& path);
	/**
	 * Open the GD-ROM drive lid.
	 */
	void openGdrom();

	Sh4Executor *getSh4Executor();

	void run(std::function<void()> func) {
		runner.runOnThread(func);
	}

	void dc_reset(bool hard); // for tests only

private:
	bool checkStatus(bool wait = false);
	void runInternal();
	void diskChange();

	enum State {
		Uninitialized = 0,
		Init,
		Loaded,
		Running,
		Error,
		Terminated,
	};
	State state = Uninitialized;
	std::shared_future<void> threadResult;
	bool resetRequested = false;
	bool singleStep = false;
	u64 startTime = 0;
	u32 stepRangeFrom = 0;
	u32 stepRangeTo = 0;
	bool stopRequested = false;
	std::mutex mutex;
	Sh4Executor *interpreter = nullptr;
	Sh4Executor *recompiler = nullptr;
	ThreadRunner runner;
};
extern Emulator emu;

int getGamePlatform(const std::string& filename);
