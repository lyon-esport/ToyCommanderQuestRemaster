/*
	tcvr: the arena pictures of the VR menu's Host a game page. See tcvr_arena_pics.h.

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "tcvr_arena_pics.h"
#include "tcvr_match.h"
#include "cfg/option.h"
#include "emulator.h"
#include "hw/mem/addrspace.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/pvr/Renderer_if.h"
#include "input/gamepad.h"
#include "input/gamepad_device.h"
#include "log/Log.h"
#include "oslib/storage.h"
#include "rend/vr/xr_host.h"
#ifdef USE_OPENXR
#include "rend/vr/xr_menu.h"
#endif
#include "stdclass.h"
#include "stbi.h"
#include "ui/gui.h"
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <thread>

// the headset's kept game frame drawn flat into an offscreen picture (gles.cpp)
bool gles_vr_grab_flat(std::vector<u8>& rgb, int& width, int& height);

namespace tcvr::arena
{
namespace
{

using Clock = std::chrono::steady_clock;

const char *const GameId = "MK-5102050";		// Toy Commander (PAL): the addresses below are that disc's

// Main RAM (tcvr/docs/RE/Addresses.md, "Arena pictures")
constexpr u32 MenuRequest = 0x8C0A5A6C;		// menu page request bits
constexpr u32 MainMenuBit = 0x20000;		// the main menu ("One player" picked first)
constexpr u32 RoomPageBit = 0x40;			// the one-player room select page
constexpr u32 RoomCursor = 0x8C0A5A78;		// the page's cursor: the room 0..7
constexpr u32 HouseTurn = 0x8C0A603C;		// the house's turn offset: 0 = the room's own angle
constexpr u32 RoomsMask = 0x8C0A5A5C;		// rooms unlocked (u16): 0xFF shows every room's name

constexpr int Fps = 60;
constexpr int SettleFrames = 36;			// 0.6 s per room (frames are the same after 0.4 s)
constexpr int TimeoutSeconds = 45;

enum class Phase { Idle, Start, Touring, Restore, Restoring };
std::atomic<int> phase { (int)Phase::Idle };
std::atomic<bool> cancelled, finished;
std::atomic<bool> tourLoad;		// the next game load is the tour's (gate)
std::atomic<bool> tourGame;		// the game loaded is the tour's (the VBlank steps run)
std::atomic<bool> restoreLoad;	// the next game load brings the player's own game back
std::atomic<int> grabWanted { -1 };	// VBlank -> render thread: grab that room now
std::atomic<int> roomsDone;			// render thread -> VBlank: rooms grabbed so far
bool failedThisRun;
bool wasRunning;
Clock::time_point startedAt, restoreAt, pageAt;
std::atomic<int64_t> pageMs { -1 };	// when the room page was reached (ms after the start)
int grabFails;
// the room page's bit, as the render thread reads the menu word (any time in a frame)
std::atomic<int> pageHits;
std::atomic<int64_t> lastPageMs;

int64_t steadyMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

void watchPage()
{
	if (RAM_SIZE == 0 || &mem_b[0] == nullptr)
		return;
	u32 word;
	memcpy(&word, &mem_b[MenuRequest & (RAM_SIZE - 1)], sizeof(word));
	const int64_t now = steadyMs();
	if (word & RoomPageBit)
	{
		pageHits++;
		lastPageMs = now;
	}
	else if (now - lastPageMs > 400)
		pageHits = 0;
}
std::array<std::shared_ptr<Picture>, Rooms> shots;	// this tour's (render thread)

std::mutex picturesMutex;
std::array<std::shared_ptr<const Picture>, Rooms> pictures;
std::string picturesFor;			// the game id the pictures were looked for
std::atomic<u32> nextId { 1 };

std::string folder() { return get_writable_data_path("arena"); }
std::string fileOf(const std::string& gameId, int room)
{
	return folder() + "/" + gameId + "_room" + std::to_string(room + 1) + ".png";
}

bool lobbyActive()
{
	return match::lobbyStarted() && match::theLobby().view().phase != lobby::Phase::Idle;
}

int64_t msSince(Clock::time_point t)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

// The pictures on disk for this game, once per game id (render thread).
void loadPictures()
{
	const std::string id = settings.content.gameId;
	if (id == picturesFor)
		return;
	picturesFor = id;
	std::array<std::shared_ptr<const Picture>, Rooms> found;
	int count = 0;
	if (id == GameId)
		for (int r = 0; r < Rooms; r++)
		{
			hostfs::File *f = hostfs::storage().openFile(fileOf(id, r), "rb");
			if (f == nullptr)
				continue;
			int w = 0, h = 0, n = 0;
			stbi_set_flip_vertically_on_load_thread(0);
			u8 *data = stbi_load_from_file(f, &w, &h, &n, STBI_rgb_alpha);
			delete f;
			if (data == nullptr)
				continue;
			auto p = std::make_shared<Picture>();
			p->width = w;
			p->height = h;
			p->rgba.assign(data, data + (size_t)w * h * 4);
			p->id = nextId++;
			stbi_image_free(data);
			found[r] = p;
			count++;
		}
	std::lock_guard<std::mutex> lock(picturesMutex);
	pictures = found;
	if (count > 0)
		NOTICE_LOG(COMMON, "ARENA: %d arena pictures found", count);
}

bool allPictures()
{
	std::lock_guard<std::mutex> lock(picturesMutex);
	return std::all_of(pictures.begin(), pictures.end(), [](const auto& p) { return p != nullptr; });
}

// A whole frame (RGB, top row first) into a PicWidth x PicHeight RGBA picture (each pixel the average of
// the frame's pixels it covers).
std::shared_ptr<Picture> shrink(const std::vector<u8>& rgb, int w, int h)
{
	auto p = std::make_shared<Picture>();
	p->width = PicWidth;
	p->height = PicHeight;
	p->rgba.resize((size_t)PicWidth * PicHeight * 4);
	for (int y = 0; y < PicHeight; y++)
	{
		const int y0 = y * h / PicHeight, y1 = std::max(y0 + 1, (y + 1) * h / PicHeight);
		for (int x = 0; x < PicWidth; x++)
		{
			const int x0 = x * w / PicWidth, x1 = std::max(x0 + 1, (x + 1) * w / PicWidth);
			u32 sum[3] {};
			int n = 0;
			for (int sy = y0; sy < y1 && sy < h; sy++)
				for (int sx = x0; sx < x1 && sx < w; sx++)
				{
					const u8 *s = &rgb[((size_t)sy * w + sx) * 3];
					sum[0] += s[0]; sum[1] += s[1]; sum[2] += s[2];
					n++;
				}
			u8 *d = &p->rgba[((size_t)y * PicWidth + x) * 4];
			for (int c = 0; c < 3; c++)
				d[c] = (u8)(n > 0 ? sum[c] / n : 0);
			d[3] = 255;
		}
	}
	p->id = nextId++;
	return p;
}

// What the game shows now (render thread)
bool grab(std::shared_ptr<Picture>& out)
{
	std::vector<u8> rgb;
	int w = 0, h = 0;
	bool ok;
	if (vr::xr::enabled())
		ok = gles_vr_grab_flat(rgb, w, h);
	else
		ok = renderer != nullptr && renderer->GetLastFrame(rgb, w, h);
	if (!ok || w <= 0 || h <= 0 || rgb.size() < (size_t)w * h * 3)
		return false;
	out = shrink(rgb, w, h);
	return true;
}

void savePictures(const std::string& gameId, const std::array<std::shared_ptr<Picture>, Rooms>& set)
{
	std::thread([gameId, set] {
		make_directory(folder());
		int saved = 0;
		for (int r = 0; r < Rooms; r++)
		{
			std::vector<u8> png;
			stbi_write_png_to_func([](void *ctx, void *data, int size) {
				auto v = static_cast<std::vector<u8> *>(ctx);
				v->insert(v->end(), (u8 *)data, (u8 *)data + size);
			}, &png, set[r]->width, set[r]->height, 4, set[r]->rgba.data(), set[r]->width * 4);
			const std::string path = fileOf(gameId, r);
			hostfs::File *f = png.empty() ? nullptr : hostfs::storage().openFile(path, "wb");
			if (f == nullptr)
				continue;
			if (f->write(png.data(), 1, png.size()) == png.size())
				saved++;
			delete f;
		}
		if (saved == Rooms)
			NOTICE_LOG(COMMON, "ARENA: the 8 arena pictures are kept in %s", folder().c_str());
		else
			WARN_LOG(COMMON, "ARENA: only %d of the 8 arena pictures could be kept", saved);
	}).detach();
}

void forgetPaused()
{
#ifdef USE_OPENXR
	vr::xr::forgetPausedGame();
#endif
}

void releaseStart()
{
	kcode[0] |= DC_BTN_START;		// (active low)
}

// ---- the game's side: the presses and the memory writes, once per frame (the emulator's thread) ----

struct Steps
{
	int frame = 0;
	int hold = 0, gap = 0;		// START held for, frames before the next press
	int mainStable = 0;			// frames in a row on the main menu
	bool mainPressed = false;	// START pressed on the main menu
	int mainPressFrame = 0;
	bool onPage = false;
	int room = -1, settle = 0;
} st;

void onEvent(Event event, void *)
{
	if (event == Event::Start)
	{
		st = Steps();
		return;
	}
	if (!tourGame || phase != (int)Phase::Touring)
		return;
	st.frame++;
	const u32 req = addrspace::read32(MenuRequest);
	if (!st.onPage && st.frame % (3 * Fps) == 0)
		NOTICE_LOG(COMMON, "ARENA: frame %d, menu word %08x, main menu for %d frames, room page %d", st.frame, req, st.mainStable, (int)st.onPage);
	if (!st.onPage)
	{
		if (st.hold > 0 && --st.hold == 0)
			releaseStart();
		// (the word read here, at the end of a frame, never has the room page's bit: the render thread's reads,
		// at any time in a frame, see it)
		if (st.mainPressed && pageHits >= 3 && steadyMs() - lastPageMs < 400)
		{
			st.onPage = true;
			st.hold = 0;
			releaseStart();
			pageMs = st.frame * 1000 / Fps;
			return;
		}
		st.mainStable = (req & MainMenuBit) != 0 ? st.mainStable + 1 : 0;
		if (st.gap > 0)
		{
			st.gap--;
			return;
		}
		// START every 1.5 s through the logos, the title and the "insert a memory card" warning; on the main
		// menu, START once it has been there 0.6 s (earlier presses are ignored) for One player, again while
		// the main menu stays. After that, no press: one more on the room page opens the mission select.
		const bool main = st.mainStable >= SettleFrames;
		if (main || (!st.mainPressed && (req & MainMenuBit) == 0 && st.frame >= 2 * Fps))
		{
			kcode[0] &= ~DC_BTN_START;
			st.hold = 9;
			st.gap = main ? Fps : 90;
			if (main)
			{
				st.mainPressed = true;
				st.mainPressFrame = st.frame;
			}
		}
		return;
	}
	const int done = roomsDone;
	if (done >= Rooms)
		return;
	// every room unlocked (its name shown, the house still), this room, at its own angle
	addrspace::write16(RoomsMask, 0x00FF);
	addrspace::write32(HouseTurn, 0);
	addrspace::write32(RoomCursor, (u32)done);
	if (done != st.room)
	{
		st.room = done;
		st.settle = 0;
	}
	if (grabWanted < 0 && ++st.settle >= SettleFrames)
		grabWanted = done;
}

// (at the first tour, not at static initialisation: the event lists may not be made yet then)
void listen()
{
	static bool listening;
	if (listening)
		return;
	listening = true;
	EventManager::listen(Event::Start, onEvent);
	EventManager::listen(Event::VBlank, onEvent);
}

bool begin(bool force)
{
	if (phase != (int)Phase::Idle)
		return true;
	if (settings.content.gameId != GameId || settings.content.path.empty() || gui_last_game_path().empty())
		return false;
	if (match::inMatch() || lobbyActive() || config::GGPOEnable)
		return false;
	if (!force)
	{
		loadPictures();
		if (allPictures() || failedThisRun)
			return false;
	}
	NOTICE_LOG(COMMON, "ARENA: picture tour starts%s", force ? " (asked)" : ": arena pictures missing");
	listen();
	cancelled = false;
	finished = false;
	phase = (int)Phase::Start;
	return true;
}

void goRestore(const char *why)
{
	NOTICE_LOG(COMMON, "ARENA: picture tour %s after %.1f s: the player's game comes back", why, msSince(startedAt) / 1000.f);
	phase = (int)Phase::Restore;
	restoreAt = Clock::now();
}

}	// namespace

bool startIfMissing() { return begin(false); }
bool forceStart() { return begin(true); }

bool busy()
{
	return phase != (int)Phase::Idle;
}

std::string progressText()
{
	const int p = phase;
	if (p == (int)Phase::Restore || p == (int)Phase::Restoring)
		return "Arena pictures ready. Back to your game...";
	const int done = roomsDone;
	if (pageMs < 0)
		return "Preparing arena pictures...";
	return "Preparing arena pictures... " + std::to_string(std::min(done + 1, Rooms)) + " / " + std::to_string(Rooms);
}

void cancel()
{
	if (phase == (int)Phase::Idle || cancelled.exchange(true))
		return;
	NOTICE_LOG(COMMON, "ARENA: picture tour cancelled");
}

bool takeFinished()
{
	return finished.exchange(false);
}

void poll()
{
	switch ((Phase)phase.load())
	{
	case Phase::Idle:
		loadPictures();
		return;
	case Phase::Start:
	{
		if (cancelled || match::inMatch() || lobbyActive())
		{
			phase = (int)Phase::Idle;
			return;
		}
		wasRunning = emu.running();
		// (no savestate written over the player's own when its game unloads: kept apart instead)
		config::AutoSaveState.override(false);
		if (!match::keepPlayerGame())
		{
			WARN_LOG(COMMON, "ARENA: the player's game could not be kept: no picture tour");
			failedThisRun = true;
			phase = (int)Phase::Idle;
			if (wasRunning && !emu.running())
				emu.start();
			return;
		}
		startedAt = Clock::now();
		pageMs = -1;
		roomsDone = 0;
		pageHits = 0;
		lastPageMs = 0;
		grabWanted = -1;
		grabFails = 0;
		shots = {};
		tourLoad = true;
		phase = (int)Phase::Touring;
		forgetPaused();
		gui_start_game(gui_last_game_path());
		return;
	}
	case Phase::Touring:
	{
		if (cancelled)
		{
			goRestore("cancelled");
			return;
		}
		if (msSince(startedAt) > TimeoutSeconds * 1000)
		{
			WARN_LOG(COMMON, "ARENA: picture tour timed out (%d of 8 rooms, room page %s)", roomsDone.load(), pageMs >= 0 ? "reached" : "not reached");
			failedThisRun = true;
			goRestore("timed out");
			return;
		}
		if (emu.running())
			watchPage();
		const int g = grabWanted;
		if (g < 0 || g != roomsDone || !emu.running())
			return;
		std::shared_ptr<Picture> p;
		if (!grab(p))
		{
			if (++grabFails > 60)
			{
				WARN_LOG(COMMON, "ARENA: the game's frame could not be read");
				failedThisRun = true;
				goRestore("failed");
			}
			return;
		}
		shots[g] = p;
		NOTICE_LOG(COMMON, "ARENA: room %d grabbed at %.1f s", g + 1, msSince(startedAt) / 1000.f);
		grabWanted = -1;
		if (++roomsDone >= Rooms)
			goRestore("done");
		return;
	}
	case Phase::Restore:
	{
		const int kept = match::playerGameKept();
		if (kept == 0 && msSince(restoreAt) < 5000)
			return;		// (its file still being written)
		if (!emu.running() && msSince(restoreAt) < 10000)
			return;		// (the tour's game still loading)
		tourGame = false;
		releaseStart();
		restoreLoad = true;
		config::AutoSaveState.override(false);	// (the tour's game is never saved)
		phase = (int)Phase::Restoring;
		forgetPaused();
		gui_start_game(gui_last_game_path());
		return;
	}
	case Phase::Restoring:
	{
		const bool back = emu.running() && !restoreLoad;
		if (!back && msSince(restoreAt) < 30000)
			return;
		const bool complete = roomsDone >= Rooms;
		NOTICE_LOG(COMMON, "ARENA: picture tour over in %.1f s (room page at %.1f s of game time, %d rooms)%s",
				msSince(startedAt) / 1000.f, pageMs / 1000.f, roomsDone.load(), back ? "" : ", the game did not come back in time");
		if (complete)
		{
			std::array<std::shared_ptr<Picture>, Rooms> set = shots;
			savePictures(settings.content.gameId, set);
			std::lock_guard<std::mutex> lock(picturesMutex);
			for (int r = 0; r < Rooms; r++)
				pictures[r] = set[r];
			picturesFor = settings.content.gameId;
		}
		shots = {};
		phase = (int)Phase::Idle;
		finished = true;
		return;
	}
	}
}

void gate()
{
	if (!tourLoad.exchange(false))
	{
		tourGame = false;
		return;
	}
	// the tour's game: no memory card (the game never writes a save, and skips the name and profile
	// pages), no savestate loaded or written. Overrides are not saved: the next load drops them.
	for (int port = 0; port < 4; port++)
		for (int slot = 0; slot < 2; slot++)
			config::MapleExpansionDevices[port][slot].override(MDT_None);
	config::AutoSaveState.override(false);
	config::AutoLoadState.override(false);
	tourGame = true;
	NOTICE_LOG(COMMON, "ARENA: the game loads for the picture tour (no memory card)");
}

bool loadHandled()
{
	if (tourGame)
		return true;		// the tour's game starts from its power-on
	if (restoreLoad.exchange(false))
		return match::restorePlayerGame();
	return false;
}

std::shared_ptr<const Picture> picture(int room)
{
	if (room < 0 || room >= Rooms)
		return nullptr;
	std::lock_guard<std::mutex> lock(picturesMutex);
	return pictures[room];
}

}
