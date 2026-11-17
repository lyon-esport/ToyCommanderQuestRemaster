/*
	tcvr: the head-driven game camera of Toy Commander VR (backlog P0-8). See xr_headcam.h and
	tcvr/docs/RE/Camera360.md.

	The renderer's "set camera" routine 0x8C0610F6 (r4 = a 12-float camera-to-world matrix) loads
	it into XMTRX (0x8C061498), saves it (0x8C0FD5BC), inverts it into the world-to-camera matrix
	(0x8C0FD58C) and builds the culling planes (0x8C060E20). Everything the game then draws and
	culls goes through those.

	The matrix layout, from the load routine's code (it fills XMTRX back to front and the vector
	routines load x, y, z back to front too, so the two reversals cancel): in memory order,
	world = M * cam + t with M[row][col] = m[3 * row + col] and t = m[9..11]. The camera's own
	axes are the matrix's COLUMNS. Camera space is x left, y up, z forward (the projection
	0x8C061F60: sx = cx - x * fx / z, sy = cy - y * fy / z).
	(The first version of this hook mixed the rows: that turned the camera about the room's
	vertical instead of its own up axis, which with the chase camera's downward pitch did not
	match the picture's turn back: the residual swim was the jitter seen worn on 2026-11-17.)

	The hook is the routine's first instruction ("sts.l pr,@-r15", replaced by the BIOS trap
	opcode, which reios_trap hands to us): for the drawing cameras of a gameplay frame, the
	camera turned by the head (C' = C * R, R a turn in the camera's own axes) is written to a
	scratch matrix in RAM the game doesn't use, and r4 points the routine at it. The game's own
	camera records are never written, so its camera logic and game state never see the head.
	The picture drawn with C' is turned back by exactly R in the VR view (gameTurn), so the
	world stays put whatever the head does, and every camera turned this way (the chase camera,
	the vehicle's look-at, the scene's copy) lands back where the stock picture had it.

	Calls left alone: the targeting (lock-on) and the turret aim use "set camera" for game
	logic; the weapons aim with the vehicle, not the head.

	Each picture is turned back by the turn of its own frame: the hook at the scene's end
	(rnd_FrameEnd 0x8C057A86, before kmRender) latches it and the render start takes it
	(tcvrTakeFrameCamTurn). (The first version noted it at the display list's start: a list
	could take the previous frame's head while its cameras had the new one, another jitter.)

	A second hook at the start of the crosshair drawing (0x8C03EEF8) puts the stock (unturned)
	world-to-camera matrix back: the crosshair, lock-on square and weapon sights are world points
	projected to the 2D HUD plane, and the HUD plane stays with the vehicle (no turn back).

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "xr_headcam.h"
#include "xr_host.h"
#include "reios/reios.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/pvr/ta_ctx.h"
#include "rend/gles/gles.h"
#include "cfg/option.h"
#include "emulator.h"
#include "log/Log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <glm/gtc/matrix_transform.hpp>

namespace vr::xr
{
namespace
{

constexpr const char *GameId = "MK-5102050";	// Toy Commander (Europe)

struct Hook
{
	u32 pc;
	u16 stock[6];	// the stock words from the hook on (the hook's own first): the code must be exactly this
};
// "set camera" (sts.l pr,@-r15 ...)
constexpr Hook SetCamera { 0x8C0610F6, { 0x4F22, 0xD320, 0x430B, 0x0009, 0xAFEC, 0x4F26 } };
// the crosshair / sights of a player (mov.l r13,@-r15 ...)
constexpr Hook Crosshair { 0x8C03EEF8, { 0x2FD6, 0x9D7A, 0xD33F, 0x04D7, 0x9078, 0x2FC6 } };
// the end of a scene: kmRender, kmEndScene (sts.l pr,@-r15 ...)
constexpr Hook FrameEnd { 0x8C057A86, { 0x4F22, 0x6343, 0x2338, 0x8D09, 0x2F46, 0xD466 } };
constexpr const Hook *Hooks[] { &SetCamera, &Crosshair, &FrameEnd };

constexpr u32 CamToWorld = 0x8C0FD5BC;
constexpr u32 WorldToCam = 0x8C0FD58C;
constexpr u32 PlayerCount = 0x8C0A72F4;	// split screen: 1..4
// 48 bytes for the turned matrix: in the boot area (IP.BIN, 0x8C008000-0x8C00FFFF) the game
// leaves alone once running; zero in every dump taken (kitchen, attract demo, a mission).
constexpr u32 Scratch = 0x8C00BF00;

// The "set camera" calls, by return address (tcvr/docs/RE/Camera360.md).
constexpr u32 SimCaller = 0x8C019278;		// end of the simulation frame (net_game_sim_frame): 3D sound, culling
constexpr u32 TurnedCallers[] {
	SimCaller,
	0x8C011F18,		// chase camera, car (cam_update_mode_a)
	0x8C01232A,		// chase camera, plane (cam_update_mode_b, a look-at on its stack)
	0x8C012622,		// chase camera, helicopter (cam_update_mode_c)
	0x8C03ED5C,		// the player's camera (cam_playerUpdateCamera)
	0x8C027DD6,		// the scene's camera the 3D is drawn with (mnu_scene_render)
	0x8C03FC1C,		// the objective arrow's camera (hud_drawObjectiveArrow)
	0x8C03FD9E,		// and the player's camera put back after it
};
// Never turned (game logic): 0x8C03B6BA turret aim (anm_vehAimTurrets), 0x8C0402B4 lock-on
// target (ai_playerFindLockTarget). Anything else is left alone and logged once.

bool installed;
u32 vblanks, simVblank = ~0u;
glm::vec2 frameHead;		// the head for every call of a game frame, taken at its first
float stockWorldToCam[12];	// the last turned call's camera, unturned, as world-to-camera
bool haveStock;
glm::vec2 frameTurn;		// the turn the cameras of the frame being drawn got (0: none turned)

float readFloat(u32 addr)
{
	const u32 v = ReadMem32_nommu(addr);
	float f;
	memcpy(&f, &v, sizeof(f));
	return f;
}

void writeFloat(u32 addr, float f)
{
	u32 v;
	memcpy(&v, &f, sizeof(v));
	WriteMem32_nommu(addr, v);
}

bool gameplayFrame()
{
	return vblanks - simVblank <= 2;
}

// What calls it (test aid): every new caller, once, to the log; and how many turned calls.
void noteCaller(u32 matrix, u32 caller, bool turned)
{
	static u32 seen[32];
	static int count;
	if (std::find(seen, seen + count, caller) == seen + count && count < (int)std::size(seen))
	{
		seen[count++] = caller;
		NOTICE_LOG(RENDERER, "VR head camera: set camera %08x from %08x%s", matrix, caller, turned ? " (turned)" : "");
	}
	static u32 turnedCalls, calls;
	static auto since = std::chrono::steady_clock::now();
	calls++;
	turnedCalls += turned;
	const auto now = std::chrono::steady_clock::now();
	if (now - since >= std::chrono::seconds(10))
	{
		INFO_LOG(RENDERER, "VR head camera: %u calls, %u turned in 10 s; head %.1f %.1f degrees", calls, turnedCalls,
				frameHead.x * 57.29578f, frameHead.y * 57.29578f);
		calls = turnedCalls = 0;
		since = now;
	}
}

bool wanted(u32 caller)
{
	if (headCameraTarget() == 3)
		return true;
	if (!gameplayFrame() || ReadMem32_nommu(PlayerCount) > 1)
		return false;
	return std::find(std::begin(TurnedCallers), std::end(TurnedCallers), caller) != std::end(TurnedCallers);
}

void setCameraTrap(Sh4Context *ctx)
{
	// the instruction the hook replaced: sts.l pr,@-r15
	ctx->r[15] -= 4;
	WriteMem32_nommu(ctx->r[15], ctx->pr);
	ctx->pc = SetCamera.pc + 2;

	const u32 matrix = ctx->r[4];
	const u32 caller = ctx->pr;
	if (caller == SimCaller)
	{
		// a new game frame: one head direction for all its cameras
		simVblank = vblanks;
		frameHead = headYawPitch();
		frameTurn = glm::vec2(0.f);
	}
	const int mode = config::VrHeadCamera;
	const bool turn = mode > 0 && wanted(caller);
	noteCaller(matrix, caller, turn);
	if (!turn)
		return;

	float m[12];
	for (int i = 0; i < 12; i++)
		m[i] = readFloat(matrix + i * 4);
	for (float f : m)
		if (!std::isfinite(f))
			return;
	// The turn, in the camera's own axes (x left, y up, z forward): the head's yaw (positive to
	// the left) about y, then its pitch (positive up) about x. The VR view's eye space is x
	// right, y up, z back, a half turn about y from the camera's: a yaw keeps its sign there, a
	// pitch changes it. So R = Ry(yaw) * Rx(-pitch) here is Ry(yaw) * Rx(pitch) in the eye
	// space, exactly the turn back gameTurn() gives the picture.
	const float yaw = frameHead.x;
	const float pitch = mode >= 2 ? std::clamp(frameHead.y, -1.4f, 1.4f) : 0.f;
	const glm::mat3 r = glm::mat3(glm::rotate(glm::mat4(1.f), yaw, glm::vec3(0.f, 1.f, 0.f))
			* glm::rotate(glm::mat4(1.f), -pitch, glm::vec3(1.f, 0.f, 0.f)));
	// M' = M * R (glm is column-major: r[col][row])
	float out[12];
	for (int row = 0; row < 3; row++)
		for (int col = 0; col < 3; col++)
			out[row * 3 + col] = m[row * 3 + 0] * r[col][0] + m[row * 3 + 1] * r[col][1] + m[row * 3 + 2] * r[col][2];
	out[9] = m[9];
	out[10] = m[10];
	out[11] = m[11];
	for (int i = 0; i < 12; i++)
		writeFloat(Scratch + i * 4, out[i]);
	ctx->r[4] = Scratch;

	// the stock camera as world-to-camera, for the HUD's projections (crosshairTrap):
	// M^T and -M^T t, same layout
	for (int row = 0; row < 3; row++)
		for (int col = 0; col < 3; col++)
			stockWorldToCam[row * 3 + col] = m[col * 3 + row];
	for (int row = 0; row < 3; row++)
		stockWorldToCam[9 + row] = -(m[0 * 3 + row] * m[9] + m[1 * 3 + row] * m[10] + m[2 * 3 + row] * m[11]);
	haveStock = true;

	frameTurn = glm::vec2(yaw, pitch);
}

void crosshairTrap(Sh4Context *ctx)
{
	// the instruction the hook replaced: mov.l r13,@-r15
	ctx->r[15] -= 4;
	WriteMem32_nommu(ctx->r[15], ctx->r[13]);
	ctx->pc = Crosshair.pc + 2;
	// The crosshair, the lock-on square and the weapon sights are world points projected with
	// the world-to-camera matrix and drawn on the 2D HUD plane: project them with the stock
	// camera, so they land on their point in the vehicle's frame (where the HUD plane is).
	// Only the matrix in RAM: the culling planes and XMTRX stay turned; the HUD draws no 3D
	// until the next "set camera" call.
	if (!haveStock || config::VrHeadCamera <= 0 || !gameplayFrame())
		return;
	for (int i = 0; i < 12; i++)
		writeFloat(WorldToCam + i * 4, stockWorldToCam[i]);
}

void frameEndTrap(Sh4Context *ctx)
{
	// the instruction the hook replaced: sts.l pr,@-r15
	ctx->r[15] -= 4;
	WriteMem32_nommu(ctx->r[15], ctx->pr);
	ctx->pc = FrameEnd.pc + 2;
	// The scene is complete: its picture (rendered from kmRender on) gets this frame's turn.
	const glm::vec2 turn = gameplayFrame() ? frameTurn : glm::vec2(0.f);
	tcvrSetFrameCamTurn(turn.x, turn.y);
}

bool trap(Sh4Context *ctx, u32 pc)
{
	if (!installed)
		return false;
	if (pc == SetCamera.pc)
		setCameraTrap(ctx);
	else if (pc == Crosshair.pc)
		crosshairTrap(ctx);
	else if (pc == FrameEnd.pc)
		frameEndTrap(ctx);
	else
		return false;
	return true;
}

bool codeIsStock()
{
	for (const Hook *h : Hooks)
		for (size_t i = 0; i < std::size(h->stock); i++)
			if (ReadMem16_nommu(h->pc + (u32)i * 2) != (i == 0 && installed ? REIOS_OPCODE : h->stock[i]))
				return false;
	return true;
}

}	// namespace

void updateHeadCamera()
{
	vblanks++;
	const bool want = settings.content.gameId == GameId && config::VrHeadCamera > 0
			&& !config::GGPOEnable && !config::NetworkEnable && !config::BattleCableEnable;
	if (want && !installed)
	{
		// (only over the stock code, so another disc or a code overlay isn't touched)
		if (codeIsStock())
		{
			reios_game_trap = trap;
			// (the dynarec drops what it compiled from the page when it's written)
			for (const Hook *h : Hooks)
				WriteMem16_nommu(h->pc, REIOS_OPCODE);
			installed = true;
			haveStock = false;
			NOTICE_LOG(RENDERER, "VR head camera: hooks in");
		}
	}
	else if (installed && (!want || !codeIsStock()))
	{
		for (const Hook *h : Hooks)
			if (ReadMem16_nommu(h->pc) == REIOS_OPCODE)
				WriteMem16_nommu(h->pc, h->stock[0]);
		installed = false;
		NOTICE_LOG(RENDERER, "VR head camera: hooks out");
	}
	if (!installed)
		tcvrSetFrameCamTurn(0.f, 0.f);
}

glm::vec2 shownCameraTurn()
{
	return gles_vr_frame_cam_turn();
}

static struct HeadCameraRegistration
{
	HeadCameraRegistration() {
		EventManager::listen(Event::VBlank, [](Event, void *) { updateHeadCamera(); });
	}
} headCameraRegistration;

}
