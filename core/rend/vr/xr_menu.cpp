/*
	tcvr: the VR menu. See xr_menu.h.

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "xr_menu.h"
#include "xr_panel.h"
#include "cfg/option.h"
#include "emulator.h"
#include "hw/pvr/Renderer_if.h"
#include "rend/CustomTexture.h"
#include "log/Log.h"
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

bool open;
bool keepRunning;			// the game keeps running behind the menu (to watch its frame rate change)
bool pausedGame;			// the menu stopped the emulator (and starts it again)
float placedYaw;			// where the head looked when it opened
int selected;
std::string status;			// the last action's result
Clock::time_point statusUntil;
int confirming = -1;		// the line waiting for a second A (import, quit)

// stick navigation, with key-repeat
int heldDir;				// 0 none, 1 up, 2 down, 3 left, 4 right
Clock::time_point nextRepeat;

// frame rates, over the last second
u32 headsetFrames;
u32 lastFrameCount;
Clock::time_point rateSince;
float headsetFps, gameFps;

// A setting with fixed steps, shown by name.
struct Choice
{
	const char *label;
	std::vector<float> values;
	std::vector<const char *> names;
	std::function<float()> get;
	std::function<void(float)> set;
};

void pauseGame();
void resumeGame();

void saveOption(config::Option<float>& o, float v) { o.set(v); o.save(); }
void saveOption(config::Option<int>& o, float v) { o.set((int)std::lround(v)); o.save(); }

const std::vector<Choice>& choices()
{
	static const std::vector<Choice> list {
		{ "Game behind the menu", { 0.f, 1.f }, { "Paused", "Running" },
			[] { return keepRunning ? 1.f : 0.f; }, [](float v) {
				keepRunning = v > 0.5f;
				if (keepRunning)
					resumeGame();
				else
					pauseGame();
			} },
		{ "Comfort vignette", { 0.f, 0.3f, 0.6f, 1.f }, { "Off", "Low", "Medium", "High" },
			[] { return (float)config::VrComfortVignette; }, [](float v) { saveOption(config::VrComfortVignette, v); } },
		{ "Anti-aliasing (MSAA)", { 1.f, 2.f, 4.f }, { "Off", "2x", "4x" },
			[] { return (float)config::VrMsaa; }, [](float v) { saveOption(config::VrMsaa, v); } },
		{ "Anisotropic filtering", { 1.f, 2.f, 4.f, 8.f, 16.f }, { "Off", "2x", "4x", "8x", "16x" },
			[] { return (float)config::AnisotropicFiltering; }, [](float v) { saveOption(config::AnisotropicFiltering, v); } },
		{ "Sharper far textures", { 0.f, -0.5f, -1.f }, { "Off", "Low (-0.5)", "High (-1.0)" },
			[] { return (float)config::VrTexLodBias; }, [](float v) { saveOption(config::VrTexLodBias, v); } },
		// the upscale cache (CustomTexture.cpp): each texture upscaled once, then reused
		{ "HD textures (upscale cache)", { 0.f, 2.f, 4.f }, { "Off", "2x", "4x" },
			[] { return (float)config::VrUpscaleCache; }, [](float v) { saveOption(config::VrUpscaleCache, v); } },
		// supersampling: more pixels per eye than the headset's recommended size, sharper
		{ "Resolution multiplier", { 1.f, 1.1f, 1.2f, 1.3f, 1.4f, 1.5f, 1.6f }, { "100%", "110%", "120%", "130%", "140%", "150%", "160%" },
			[] { return (float)config::VrXrResolution; }, [](float v) { saveOption(config::VrXrResolution, v); } },
	};
	return list;
}

// The lines: Resume, Recenter, World size, the choices, Export, Import, Quit.
enum Line { Resume, Recenter, WorldSize, FirstChoice };
int exportLine() { return FirstChoice + (int)choices().size(); }
int importLine() { return exportLine() + 1; }
int quitLine() { return exportLine() + 2; }
int lineCount() { return quitLine() + 1; }

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

void openMenu(float headYaw)
{
	open = true;
	placedYaw = headYaw;
	selected = Resume;
	confirming = -1;
	heldDir = 0;
	status.clear();
	if (!keepRunning)
		pauseGame();
	NOTICE_LOG(RENDERER, "XR menu: open (game %s)", pausedGame ? "paused" : "not running");
}

std::string request(MenuRequest r)
{
	if (menuRequestHook == nullptr)
		return "Not available on this platform";
	return menuRequestHook(r);
}

void change(int line, int step)
{
	if (line == WorldSize)
	{
		const float scale = std::clamp(config::VrWorldScale * (step > 0 ? 1.1f : 1.f / 1.1f), 0.005f, 0.2f);
		saveOption(config::VrWorldScale, scale);
		return;
	}
	if (line >= FirstChoice && line < exportLine())
	{
		const Choice& c = choices()[line - FirstChoice];
		const int i = std::clamp(nearestIndex(c) + step, 0, (int)c.values.size() - 1);
		c.set(c.values[i]);
		NOTICE_LOG(RENDERER, "XR menu: %s = %s", c.label, c.names[i]);
	}
}

MenuAction accept(int line)
{
	if (line == Resume)
	{
		closeMenu();
		return MenuAction::None;
	}
	if (line == Recenter)
	{
		closeMenu();
		return MenuAction::Recenter;
	}
	if (line == WorldSize || (line >= FirstChoice && line < exportLine()))
	{
		change(line, 1);
		return MenuAction::None;
	}
	if (line == exportLine())
	{
		setStatus(request(MenuRequest::ExportSaves));
		return MenuAction::None;
	}
	// import and quit: A twice
	if (confirming != line)
	{
		confirming = line;
		setStatus(line == quitLine() ? "Press A again to quit" : "Press A again: replaces your saves and restarts the game");
		return MenuAction::None;
	}
	confirming = -1;
	setStatus(request(line == quitLine() ? MenuRequest::Quit : MenuRequest::ImportSaves));
	return MenuAction::None;
}

std::string lineText(int line)
{
	char buf[96];
	if (line == Resume)
		return "Resume";
	if (line == Recenter)
		return "Recenter view";
	if (line == WorldSize)
	{
		snprintf(buf, sizeof(buf), "World size:  %d%%", (int)std::lround(config::VrWorldScale / 0.11f * 100.f));
		return buf;
	}
	if (line < exportLine())
	{
		const Choice& c = choices()[line - FirstChoice];
		snprintf(buf, sizeof(buf), "%s:  %s", c.label, c.names[nearestIndex(c)]);
		return buf;
	}
	if (line == exportLine())
		return "Export saves to Download";
	if (line == importLine())
		return "Import saves from Download";
	return "Quit game";
}

}	// namespace

bool menuOpen()
{
	return open;
}

void closeMenu()
{
	if (!open)
		return;
	open = false;
	confirming = -1;
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
		gameFps = (FrameCount - lastFrameCount) / secs;
		headsetFrames = 0;
		lastFrameCount = FrameCount;
		rateSince = now;
	}
}

MenuAction updateMenu(const MenuInput& input, float headYaw)
{
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
		closeMenu();
		return MenuAction::None;
	}
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
	if (step)
	{
		if (dir == 1 || dir == 2)
		{
			selected = (selected + (dir == 1 ? -1 : 1) + lineCount()) % lineCount();
			confirming = -1;
		}
		else
			change(selected, dir == 3 ? -1 : 1);
	}
	if (input.accept)
		return accept(selected);
	return MenuAction::None;
}

void drawMenu(const glm::mat4& viewProj)
{
	if (!open)
		return;
	const glm::vec3 red(0.86f, 0.13f, 0.1f), white(0.95f), grey(0.6f), yellow(1.f, 0.85f, 0.3f);
	Panel panel;
	panel.lines.push_back({ "TOY COMMANDER VR", 0.05f, red });
	for (int i = 0; i < lineCount(); i++)
	{
		std::string s = lineText(i);
		const bool sel = i == selected;
		const bool hasValue = i == WorldSize || (i >= FirstChoice && i < exportLine());
		if (sel)
			s = hasValue ? "<   " + s + "   >" : ">  " + s + "  <";
		panel.lines.push_back({ s, 0.034f, sel ? (confirming == i ? yellow : white) : grey });
	}
	char rates[96];
	int upscaled, queued, cached;
	custom_texture.getUpscaleCacheProgress(upscaled, queued, cached);
	snprintf(rates, sizeof(rates), "Headset %.0f fps   Game %.0f fps%s   HD textures %d", headsetFps, gameFps,
			pausedGame ? " (paused)" : "", cached);
	const bool showStatus = !status.empty() && Clock::now() < statusUntil;
	panel.lines.push_back({ showStatus ? status : std::string(rates), 0.031f, showStatus ? yellow : grey });
	// in front of where the head looked when it opened
	drawPanel(viewProj * glm::rotate(glm::mat4(1.f), placedYaw, glm::vec3(0.f, 1.f, 0.f)), panel);
}

}
