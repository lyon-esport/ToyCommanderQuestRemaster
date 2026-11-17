/*
	tcvr: the VR menu. See xr_menu.h.

	Grouped in sections (Manu, 2026-11-16): the top page has Resume, Recenter view, one line per
	section (Display, Image quality, Performance, Comfort, Saves) and Quit game; A opens a section,
	"Back" or B returns to the top page, B on the top page closes the menu. A section without any
	line in this build isn't shown.

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

enum class Section { Display, Image, Performance, Comfort, Saves, Count };
const char *const SectionNames[] = { "Display", "Image quality", "Performance", "Comfort", "Saves" };

bool open;
bool pausedGame;			// the menu stopped the emulator (and starts it again)
float placedYaw;			// where the head looked when it opened
int page = -1;				// -1: the top page, else a Section
int selected;
std::string status;			// the last action's result
Clock::time_point statusUntil;
int confirming = -1;		// the action waiting for a second A (import, quit)

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
		// the frame-rate counter at the bottom right of the view
		{ Section::Performance, "Show frame rate", { 0.f, 1.f }, { "Off", "On" },
			[] { return config::VrShowFps ? 1.f : 0.f; }, [](float v) { config::VrShowFps.set(v > 0.5f); config::VrShowFps.save(); } },
		// tcvr: fixed foveated rendering: fewer pixels at the lens edges, more frame rate
		{ Section::Performance, "Fixed foveation (edges)", { 0.f, 1.f, 2.f, 3.f }, { "Off", "Low", "Medium", "High" },
			[] { return (float)config::VrFoveation; }, [](float v) { saveOption(config::VrFoveation, v); } },
		// tcvr: the other frame-rate tweaks, each switchable to compare (both on by default)
		{ Section::Performance, "Upload frame once", { 0.f, 1.f }, { "Off", "On" },
			[] { return config::VrUploadOnce ? 1.f : 0.f; }, [](float v) { config::VrUploadOnce.set(v > 0.5f); config::VrUploadOnce.save(); } },
		{ Section::Performance, "Discard eye depth", { 0.f, 1.f }, { "Off", "On" },
			[] { return config::VrDiscardDepth ? 1.f : 0.f; }, [](float v) { config::VrDiscardDepth.set(v > 0.5f); config::VrDiscardDepth.save(); } },
		{ Section::Comfort, "Comfort vignette", { 0.f, 0.3f, 0.6f, 1.f }, { "Off", "Low", "Medium", "High" },
			[] { return (float)config::VrComfortVignette; }, [](float v) { saveOption(config::VrComfortVignette, v); } },
		// tcvr: the head-driven camera (xr_headcam.cpp): look all around the vehicle
		{ Section::Comfort, "Head turns the camera", { 0.f, 1.f, 2.f }, { "Off", "Left/right", "Left/right + up/down" },
			[] { return (float)config::VrHeadCamera; }, [](float v) { saveOption(config::VrHeadCamera, v); } },
		{ Section::Comfort, "Sight when looking away", { 0.f, 1.f }, { "On the vehicle's aim", "Hidden" },
			[] { return (float)config::VrHeadCameraSight; }, [](float v) { saveOption(config::VrHeadCameraSight, v); } },
	};
	return list;
}

// A line of the page shown.
enum class Action { Resume, Recenter, OpenSection, Back, Choice, WorldSize, Export, Import, Quit };
struct Entry
{
	Action action;
	int index = -1;		// the choice (Choice) or the section (OpenSection)
};

std::vector<Entry> entries()
{
	std::vector<Entry> list;
	const auto& all = choices();
	if (page < 0)
	{
		list.push_back({ Action::Resume });
		list.push_back({ Action::Recenter });
		for (int s = 0; s < (int)Section::Count; s++)
		{
			bool any = (Section)s == Section::Display || (Section)s == Section::Saves;
			for (const Choice& c : all)
				any = any || c.section == (Section)s;
			if (any)
				list.push_back({ Action::OpenSection, s });
		}
		list.push_back({ Action::Quit });
		return list;
	}
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
	}
	return list;
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

void openMenu(float headYaw)
{
	open = true;
	placedYaw = headYaw;
	page = -1;
	selected = 0;
	confirming = -1;
	heldDir = 0;
	status.clear();
	// always frozen behind the menu (Manu): the game must not go on without the player
	pauseGame();
	NOTICE_LOG(RENDERER, "XR menu: open (game %s)", pausedGame ? "paused" : "not running");
}

void showPage(int newPage)
{
	const int from = page;
	page = newPage;
	confirming = -1;
	selected = 0;
	if (newPage < 0)
	{
		// back on the top page: on the section left
		const auto list = entries();
		for (int i = 0; i < (int)list.size(); i++)
			if (list[i].action == Action::OpenSection && list[i].index == from)
				selected = i;
	}
	else
		selected = 1;	// the section's first setting, below Back
	if (newPage >= 0)
		NOTICE_LOG(RENDERER, "XR menu: section %s", SectionNames[newPage]);
}

std::string request(MenuRequest r)
{
	if (menuRequestHook == nullptr)
		return "Not available on this platform";
	return menuRequestHook(r);
}

void change(const Entry& e, int step)
{
	if (e.action == Action::WorldSize)
	{
		const float scale = std::clamp(config::VrWorldScale * (step > 0 ? 1.1f : 1.f / 1.1f), 0.005f, 0.2f);
		saveOption(config::VrWorldScale, scale);
		return;
	}
	if (e.action == Action::Choice)
	{
		const Choice& c = choices()[e.index];
		const int i = std::clamp(nearestIndex(c) + step, 0, (int)c.values.size() - 1);
		c.set(c.values[i]);
		NOTICE_LOG(RENDERER, "XR menu: %s = %s", c.label, c.names[i]);
	}
}

MenuAction accept(const Entry& e)
{
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
		showPage(-1);
		return MenuAction::None;
	case Action::Choice:
	case Action::WorldSize:
		change(e, 1);
		return MenuAction::None;
	case Action::Export:
		setStatus(request(MenuRequest::ExportSaves));
		return MenuAction::None;
	case Action::Import:
	case Action::Quit:
		// A twice
		if (confirming != (int)e.action)
		{
			confirming = (int)e.action;
			setStatus(e.action == Action::Quit ? "Press A again to quit" : "Press A again: replaces your saves and restarts the game");
			return MenuAction::None;
		}
		confirming = -1;
		setStatus(request(e.action == Action::Quit ? MenuRequest::Quit : MenuRequest::ImportSaves));
		return MenuAction::None;
	}
	return MenuAction::None;
}

bool hasValue(const Entry& e)
{
	return e.action == Action::Choice || e.action == Action::WorldSize;
}

std::string lineText(const Entry& e)
{
	char buf[96];
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
		snprintf(buf, sizeof(buf), "%s:  %s", c.label, c.names[nearestIndex(c)]);
		return buf;
	}
	case Action::Export: return "Export saves to Download";
	case Action::Import: return "Import saves from Download";
	case Action::Quit: return "Quit game";
	}
	return "";
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

void closeMenu()
{
	if (!open)
		return;
	open = false;
	confirming = -1;
	page = -1;
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
		// B: out of a section, or the menu closes
		if (page >= 0)
			showPage(-1);
		else
			closeMenu();
		return MenuAction::None;
	}
	const auto list = entries();
	selected = std::clamp(selected, 0, (int)list.size() - 1);
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
		const int count = (int)list.size();
		if (dir == 1 || dir == 2)
		{
			selected = (selected + (dir == 1 ? -1 : 1) + count) % count;
			confirming = -1;
		}
		else if (hasValue(list[selected]))
			change(list[selected], dir == 3 ? -1 : 1);
		else if (dir == 4 && list[selected].action == Action::OpenSection)
			showPage(list[selected].index);		// right opens a section too
		else if (dir == 3 && page >= 0)
			showPage(-1);						// left goes back
	}
	if (input.accept)
		return accept(entries()[std::clamp(selected, 0, (int)entries().size() - 1)]);
	return MenuAction::None;
}

void drawMenu(const glm::mat4& viewProj)
{
	if (!open)
		return;
	const glm::vec3 red(0.86f, 0.13f, 0.1f), white(0.95f), grey(0.6f), yellow(1.f, 0.85f, 0.3f), soft(0.8f, 0.8f, 0.85f);
	Panel panel;
	panel.lines.push_back({ "TOY COMMANDER VR", 0.05f, red });
	panel.lines.push_back({ page < 0 ? "VR menu" : SectionNames[page], 0.04f, soft });
	const auto list = entries();
	for (int i = 0; i < (int)list.size(); i++)
	{
		std::string s = lineText(list[i]);
		const bool sel = i == selected;
		if (sel)
			s = hasValue(list[i]) ? "<   " + s + "   >" : ">  " + s + "  <";
		panel.lines.push_back({ s, 0.042f, sel ? (confirming == (int)list[i].action ? yellow : white) : grey });
	}
	char rates[96];
	int upscaled, queued, cached;
	custom_texture.getUpscaleCacheProgress(upscaled, queued, cached);
	snprintf(rates, sizeof(rates), "Headset %.0f fps   Game %.0f fps%s   HD textures %d", headsetFps, gameFps,
			pausedGame ? " (paused)" : "", cached);
	const bool showStatus = !status.empty() && Clock::now() < statusUntil;
	panel.lines.push_back({ showStatus ? status : std::string(rates), 0.034f, showStatus ? yellow : grey });
	// in front of where the head looked when it opened
	drawPanel(viewProj * glm::rotate(glm::mat4(1.f), placedYaw, glm::vec3(0.f, 1.f, 0.f)), panel);
}

}
