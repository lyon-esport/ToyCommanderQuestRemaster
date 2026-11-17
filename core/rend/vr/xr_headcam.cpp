/*
	tcvr: the head-driven game camera of Toy Commander VR (backlog P0-8). See xr_headcam.h and
	tcvr/docs/RE/Camera360.md.

	The game's cameras are records from 0x8C0D5904 (one per player; 0x8C03FFA8 returns the one in
	use). Each holds two 3x4 camera-to-world matrices (rows: right, up, back, then the position;
	y up, right-handed): at +0x154 and at +0x184. The renderer's "set camera" routine 0x8C0610F6
	(r4 = such a matrix) loads it into XMTRX (0x8C061498), then at 0x8C0610DA saves it
	(0x8C0FD5BC), inverts it into the view matrix (0x8C0FD58C) and makes the culling planes from
	them (0x8C060E20). Everything the game then draws and culls goes through those.

	The hook is the routine's first instruction, 0x8C0610F6 ("sts.l pr,@-r15", replaced by the
	BIOS trap opcode, which reios_trap hands to us): for the 3D view's call, the camera turned by
	the head (yaw about its own up axis, then pitch about its right axis; the position stays) is
	written to a scratch matrix in RAM the game doesn't use, and r4 points the routine at it. The
	game's own camera record is left alone, so its camera logic (the chase, smoothing) and its
	game state never see the head.
	(A first try at 0x8C0610DA rewrote XMTRX in the CPU context after the load: the RAM dump
	showed the routine still saved the stock matrix. The dynarec keeps the FPU registers it
	loaded in host registers there; the start of the routine is a block start, every register
	is in the context.)

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

namespace vr::xr
{
namespace
{

constexpr const char *GameId = "MK-5102050";	// Toy Commander (Europe)
constexpr u32 HookPc = 0x8C0610F6;
// The stock words from the hook on (the hook's own first): the code must be exactly this.
constexpr u16 Stock[] { 0x4F22, 0xD320, 0x430B, 0x0009, 0xAFEC, 0x4F26 };
constexpr u32 CameraRecords = 0x8C0D5904;
constexpr u32 MatrixA = 0x154, MatrixB = 0x184;
// 48 bytes for the turned matrix: in the boot area (IP.BIN, 0x8C008000-0x8C00FFFF) the game
// leaves alone once running; zero in every dump taken (kitchen, attract demo, a mission).
constexpr u32 Scratch = 0x8C00BF00;

bool installed;

// The 3D view's own call: 0x8C019268 gets the camera in use (0x8C03FFA8: a record from
// 0x8C0D5904, or another one the game points to) and calls "set camera" with its matrix B, so
// the return address is 0x8C019278 whatever record it is. (Not verified on the headset yet:
// the first build matched player 1's record by address and the picture didn't turn.)
constexpr u32 ViewCaller = 0x8C019278;
// Then the same camera is set again for the drawing (from 0x8C03ED5C, 0x8C0402B4, ...): every
// later call with the matrix the view's call had gets the same turn, else the second call
// undid the first (the first headset build: the picture turned, the game camera didn't).
u32 viewMatrix;
// And the call that sets the camera the 3D is drawn with: right after the view's, from 0x8C027DD2
// (returns to 0x8C027DD6), with a copy of the camera elsewhere in RAM (0x8CB8E738 in the
// kitchen). Headset test 2026-11-17: turning only the calls above changed nothing on screen,
// turning every call turned the view; this one is what the 3D goes through.
constexpr u32 DrawCaller = 0x8C027DD6;
// That call also draws the title and menus' 3D: it is turned only in a frame where the 3D
// view's own call came first (in a mission or the attract demo), so menus stay put.
u32 vblanks, viewVblank = ~0u;

// Which "set camera" calls get the head's turn (test aid, vr::xr::headCameraTarget()):
// 0 the 3D view's call (the default), 1 player 1's record's matrix B, 2 its A or B,
// 3 every call.
bool wanted(u32 matrix, u32 caller)
{
	switch (headCameraTarget())
	{
	case 3:
		return true;
	case 2:
		return matrix == CameraRecords + MatrixA || matrix == CameraRecords + MatrixB;
	case 1:
		return matrix == CameraRecords + MatrixB;
	default:
		if (caller == ViewCaller)
		{
			viewMatrix = matrix;
			viewVblank = vblanks;
		}
		if (vblanks - viewVblank > 2)
			return false;
		return matrix == viewMatrix || caller == DrawCaller;
	}
}

// What calls it (test aid): every new matrix address and caller, once, to the log; and how many
// turned calls a second.
void noteCaller(u32 matrix, u32 caller, bool turned)
{
	struct Seen { u32 matrix, caller; };
	static Seen seen[32];
	static int count;
	bool known = false;
	for (int i = 0; i < count && !known; i++)
		known = seen[i].matrix == matrix && seen[i].caller == caller;
	if (!known && count < (int)std::size(seen))
	{
		seen[count++] = { matrix, caller };
		NOTICE_LOG(RENDERER, "VR head camera: set camera %08x from %08x%s", matrix, caller, turned ? " (turned)" : "");
	}
	static u32 turnedCalls, calls;
	static auto since = std::chrono::steady_clock::now();
	calls++;
	turnedCalls += turned;
	const auto now = std::chrono::steady_clock::now();
	if (now - since >= std::chrono::seconds(10))
	{
		const glm::vec2 head = headYawPitch();
		INFO_LOG(RENDERER, "VR head camera: %u calls, %u turned in 10 s; head %.1f %.1f degrees", calls, turnedCalls,
				head.x * 57.29578f, head.y * 57.29578f);
		calls = turnedCalls = 0;
		since = now;
	}
}

float readFloat(u32 addr)
{
	const u32 v = ReadMem32_nommu(addr);
	float f;
	memcpy(&f, &v, sizeof(f));
	return f;
}

bool trap(Sh4Context *ctx, u32 pc)
{
	if (pc != HookPc || !installed)
		return false;
	// the instruction the hook replaced: sts.l pr,@-r15
	ctx->r[15] -= 4;
	WriteMem32_nommu(ctx->r[15], ctx->pr);
	ctx->pc = pc + 2;

	const u32 matrix = ctx->r[4];
	const int mode = config::VrHeadCamera;
	const bool turn = mode > 0 && wanted(matrix, ctx->pr);
	noteCaller(matrix, ctx->pr, turn);
	if (!turn)
		return true;
	const glm::vec2 head = headYawPitch();
	const float yaw = head.x;
	const float pitch = mode >= 2 ? std::clamp(head.y, -1.4f, 1.4f) : 0.f;
	float m[12];
	for (int i = 0; i < 12; i++)
		m[i] = readFloat(matrix + i * 4);
	if (!std::isfinite(m[0]) || !std::isfinite(m[4]) || !std::isfinite(m[8]))
		return true;
	const glm::vec3 r(m[0], m[1], m[2]), u(m[3], m[4], m[5]), b(m[6], m[7], m[8]);
	// the camera turned in its own frame by Ry(yaw) * Rx(pitch): each new axis is a column of
	// that matrix in the old axes
	const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
	const glm::vec3 r2 = cy * r - sy * b;
	const glm::vec3 u2 = sy * sp * r + cp * u + cy * sp * b;
	const glm::vec3 b2 = sy * cp * r - sp * u + cy * cp * b;
	const float out[12] { r2.x, r2.y, r2.z, u2.x, u2.y, u2.z, b2.x, b2.y, b2.z, m[9], m[10], m[11] };
	for (int i = 0; i < 12; i++)
	{
		u32 v;
		memcpy(&v, &out[i], sizeof(v));
		WriteMem32_nommu(Scratch + i * 4, v);
	}
	ctx->r[4] = Scratch;
	tcvrNoteCamTurn(yaw, pitch);
	return true;
}

bool codeIsStock()
{
	for (size_t i = 0; i < std::size(Stock); i++)
		if (ReadMem16_nommu(HookPc + (u32)i * 2) != (i == 0 && installed ? REIOS_OPCODE : Stock[i]))
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
			WriteMem16_nommu(HookPc, REIOS_OPCODE);
			installed = true;
			NOTICE_LOG(RENDERER, "VR head camera: hook in at %08x", HookPc);
		}
	}
	else if (installed && (!want || !codeIsStock()))
	{
		if (ReadMem16_nommu(HookPc) == REIOS_OPCODE)
			WriteMem16_nommu(HookPc, Stock[0]);
		installed = false;
		NOTICE_LOG(RENDERER, "VR head camera: hook out");
	}
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
