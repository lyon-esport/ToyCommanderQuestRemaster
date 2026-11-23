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
#include "cfg/option.h"
#include "libretro_core_option_defines.h"

namespace config {

// Dynarec

Option<bool> DynarecEnabled("", true);
IntOption Sh4Clock(CORE_OPTION_NAME "_sh4clock", 200);

// General

Option<int> Cable("", 3);									// TV Composite
Option<int> Region(CORE_OPTION_NAME "_region", 1);			// USA
Option<int> Broadcast(CORE_OPTION_NAME "_broadcast", 0);	// NTSC
Option<int> Language(CORE_OPTION_NAME "_language", 1);		// English
Option<bool> AutoLoadState("");
Option<bool> AutoSaveState("");
Option<int, false> SavestateSlot("");
Option<bool> ForceFreePlay(CORE_OPTION_NAME "_force_freeplay", true);

// Sound

Option<bool> DSPEnabled(CORE_OPTION_NAME "_enable_dsp", false);
#if HOST_CPU == CPU_ARM
Option<int> AudioBufferSize("", 5644);	// 128 ms
#else
Option<int> AudioBufferSize("", 2822);	// 64 ms
#endif
Option<bool> AutoLatency("");

OptionString AudioBackend("", "auto");
Option<bool> VmuSound(CORE_OPTION_NAME "_vmu_sound", false);

// Rendering

RendererOption RendererType;
Option<bool> UseMipmaps(CORE_OPTION_NAME "_mipmapping", true);
Option<bool> Widescreen(CORE_OPTION_NAME "_widescreen_hack");
Option<bool> SuperWidescreen("");
Option<bool> ShowFPS("");
Option<bool> RenderToTextureBuffer(CORE_OPTION_NAME "_enable_rttb");
Option<bool> TranslucentPolygonDepthMask("");
Option<bool> ModifierVolumes(CORE_OPTION_NAME "_volume_modifier_enable", true);
IntOption TextureUpscale(CORE_OPTION_NAME "_texupscale", 1);
IntOption MaxFilteredTextureSize(CORE_OPTION_NAME "_texupscale_max_filtered_texture_size", 256);
Option<float> ExtraDepthScale("", 1.f);
Option<bool> CustomTextures(CORE_OPTION_NAME "_custom_textures");
Option<bool> PreloadCustomTextures(CORE_OPTION_NAME "_preload_custom_textures");
Option<bool> DumpTextures(CORE_OPTION_NAME "_dump_textures");
Option<bool> DumpUniqueTextures(CORE_OPTION_NAME "_dump_unique_textures");
Option<bool> DumpReplacedTextures(CORE_OPTION_NAME "_dump_replaced_textures");
Option<int> ScreenStretching("", 100);
Option<bool> Fog(CORE_OPTION_NAME "_fog", true);
Option<bool> FloatVMUs("");
Option<bool> Rotate90("");
Option<bool> PerStripSorting("");
Option<bool> DelayFrameSwapping(CORE_OPTION_NAME "_delay_frame_swapping");
Option<bool> WidescreenGameHacks(CORE_OPTION_NAME "_widescreen_cheats");
std::array<Option<int>, 4> CrosshairColor {
	Option<int>(""),
	Option<int>(""),
	Option<int>(""),
	Option<int>(""),
};
Option<int> SkipFrame(CORE_OPTION_NAME "_frame_skipping");
Option<int> MaxThreads("", 3);
Option<int> AutoSkipFrame(CORE_OPTION_NAME "_auto_skip_frame", 0);
Option<int> RenderResolution("", 480);
Option<bool> IntegerScale("");
Option<bool> LinearInterpolation("", true);
Option<bool> VSync("", true);
Option<bool> ThreadedRendering(CORE_OPTION_NAME "_threaded_rendering", true);
Option<int> AnisotropicFiltering(CORE_OPTION_NAME "_anisotropic_filtering");
Option<int> TextureFiltering(CORE_OPTION_NAME "_texture_filtering");
Option<bool> PowerVR2Filter(CORE_OPTION_NAME "_pvr2_filtering");
Option<int64_t> PixelBufferSize("", 512_MB);
IntOption PerPixelLayers(CORE_OPTION_NAME "_oit_layers");
Option<bool> NativeDepthInterpolation(CORE_OPTION_NAME "_native_depth_interpolation");
Option<bool> EmulateFramebuffer(CORE_OPTION_NAME "_emulate_framebuffer", false);
Option<bool> FixUpscaleBleedingEdge(CORE_OPTION_NAME "_fix_upscale_bleeding_edge", true);

// Misc

Option<bool> SerialConsole("");
Option<bool> SerialPTY("");
Option<bool> UseReios(CORE_OPTION_NAME "_hle_bios");

Option<bool> OpenGlChecks("", false);
Option<bool> FastGDRomLoad(CORE_OPTION_NAME "_gdrom_fast_loading", false);
Option<bool> RamMod32MB(CORE_OPTION_NAME "_dc_32mb_mod", false);

//Option<std::vector<std::string>, false> ContentPath("");
//Option<bool, false> HideLegacyNaomiRoms("", true);

// Network

Option<bool> NetworkEnable("", false);
Option<bool> ActAsServer("", false);
Option<bool> NaomiSatellite("", false);
OptionString DNS("", "dns.flyca.st");
OptionString NetworkServer("", "");
Option<int> LocalPort("", 0);
Option<bool> EmulateBBA(CORE_OPTION_NAME "_emulate_bba", false);
Option<bool> EnableUPnP(CORE_OPTION_NAME "_upnp", true);
Option<bool> GGPOEnable("", false);
Option<int> GGPODelay("", 0);
Option<bool> NetworkStats("", false);
Option<int> GGPOAnalogAxes("", 0);
Option<bool> NetworkOutput(CORE_OPTION_NAME "_network_output", false);
Option<int> MultiboardSlaves("", 0);
Option<bool> BattleCableEnable("", false);
Option<bool> UseDCNet(CORE_OPTION_NAME "_dcnet", false);
OptionString ISPUsername("", "flycast1");

// Maple

Option<int> MouseSensitivity("", 100);
Option<int> VirtualGamepadVibration("", 20);

std::array<Option<MapleDeviceType>, 4> MapleMainDevices {
	Option<MapleDeviceType>("", MDT_None),
	Option<MapleDeviceType>("", MDT_None),
	Option<MapleDeviceType>("", MDT_None),
	Option<MapleDeviceType>("", MDT_None),
};
std::array<std::array<Option<MapleDeviceType>, 2>, 4> MapleExpansionDevices {
	Option<MapleDeviceType>("", MDT_None),
	Option<MapleDeviceType>("", MDT_None),

	Option<MapleDeviceType>("", MDT_None),
	Option<MapleDeviceType>("", MDT_None),

	Option<MapleDeviceType>("", MDT_None),
	Option<MapleDeviceType>("", MDT_None),

	Option<MapleDeviceType>("", MDT_None),
	Option<MapleDeviceType>("", MDT_None),
};

std::array<std::array<Option<int>, 2>, 4> NetworkExpansionDevices{{
	{{Option<int>("", 0),
	Option<int>("", 0)}},

	{{Option<int>("", 0),
	Option<int>("", 0)}},

	{{Option<int>("", 0),
	Option<int>("", 0)}},

	{{Option<int>("", 0),
	Option<int>("", 0)}},
}};
Option<bool> UsePhysicalVmuMemory(CORE_OPTION_NAME "_linked_vmu_storage", false);

// tcvr: our options used by the portable core. The browser build is a libretro core, which
// takes its options from this file: same defaults as the native builds (generated from
// core/cfg/option.cpp); the headset-only ones are not used but must exist.
Option<bool> VrReproject("", false);
Option<float> VrFocal("", 640.f);	// the game's stock focal length, in DC pixels
Option<int> VrProjAddr("", 0);
Option<int> VrViewportAddr("", 0);
Option<float> VrFovScale("", 1.f);	// widen the game's view by this factor (tan of the half angle)
Option<float> VrHudW("", 1.f);		// the W the game draws its 2D overlay at (sprites, text, letterbox)
Option<bool> VrHideLetterbox("", true);	// drop opaque black overlay (cinematic bars)
Option<float> VrHudDepth("", 40.f);	// game units in front of the game camera
Option<float> VrYaw("", 0.f);			// degrees
Option<float> VrPitch("", 0.f);		// degrees
Option<float> VrOffsetX("", 0.f);	// game units
Option<float> VrOffsetY("", 0.f);
Option<float> VrOffsetZ("", 0.f);
Option<bool> VrAnimate("", false);		// sway the free camera by the amplitudes above
Option<int> VrUdpPort("", 0);	// lightgun input over UDP on 127.0.0.1, 0 = off
#ifdef __ANDROID__
Option<bool> VrXr("", true);			// immersive view in the headset
#else
Option<bool> VrXr("", false);			// PCVR (OpenXR), when asked: the PCVR zip's emu.cfg does
#endif
Option<bool> VrXrGun("", true);		// the controllers drive the light gun (the one that fires holds it)
Option<bool> VrWiden("", true);		// let the game profile widen the view (vr.FovScale)
Option<float> VrWorldScale("", 0.2f);	// tcvr default 0.2 (VR menu world size 182%, its maximum); metres per game unit (HOTD2's camera rides 15 units over the street: eye height)
Option<float> VrXrResolution("", 1.3f);	// eye buffer size, times the recommended size (tcvr default 1.3, chosen on the headset 2026-11-17)
Option<int> VrMsaa("", 2);		// smooth edges: multisampling, e.g. 4 (the Quest only; 1: off)
Option<int> VrXrRefreshRate("", 120);	// Hz (tcvr default: 120)
Option<bool> VrLaser("", true);		// aim line and dot from the gun
Option<bool> VrShowGun("", true);	// a gun in the shooting hand
Option<float> VrGunScale("", 0.68f);	// its size, times the model's (24.7 cm long); 0.68: 17 cm, a compact pistol
Option<bool> VrGameHands("", true);	// the agent's own hands and pistol from the game, when files/hands.bin is there
Option<float> VrHandScale("", 1.f);	// their size, times the ripped model's (a 20 cm pistol)
Option<bool> VrSlideReload("", true);	// with them, the other hand racks the slide to reload
Option<bool> VrStaffHand("", false);	// the hero's glove around his staff (The Maze of the Kings)
Option<int> VrDebugShots("", 0);	// PC test aid: a screenshot every N seconds of game time (0: off)
Option<bool> VrDualWield("", true);	// the left controller's Start: player 2 joins, a gun in each hand
Option<bool> VrHideP2Prompt("", true);	// no "PRESS START BUTTON" for player 2 floating in the room
Option<bool> VrHandsPrep("", false);	// PC: with no hands model, run the game to its game over scene for one (the headset always does)
Option<float> VrComfortStart("", 0.8f);	// metres; closer things get pulled back (0: off)
Option<float> VrComfortMin("", 0.4f);	// metres; ...but never closer than this
Option<bool> VrAimWidened("", false);	// aim on the widened view (the game's hit test uses its stock view)
Option<bool> VrWidenFov("", true);	// widen through the game's field of view when its profile knows where
Option<bool> VrDropShotMarker("", true);	// hide the game's own 2D shot marker after shots into 3D
Option<bool> VrDropShotFlash("", true);	// hide a bright full-screen flash right after any shot
Option<bool> VrForce60Hz("", true);	// tcvr: run Toy Commander (Europe) at 60 Hz (NTSC broadcast), not 50 Hz PAL
Option<bool> VrGame60("", true);	// tcvr: Toy Commander draws 60 pictures/s in missions too (stock: 30); attract demos stay at 30
Option<bool> VrShowFps("", true);	// tcvr: frame-rate counter (default on since 2026-11-17) at the bottom right of the view
Option<float> VrTexLodBias("", 0.f);	// tcvr: texture mipmap bias, negative = sharper far textures (e.g. -0.5; 0: stock)
Option<bool> VrUploadOnce("", true);	// tcvr: the kept game frame's vertices go to the GPU once per game picture, not per eye
Option<bool> VrDiscardDepth("", true);	// tcvr: drop each eye's depth/stencil after drawing (not written out to memory)
Option<int> VrFoveation("", 1);	// tcvr: fixed foveated rendering, fewer pixels at the lens edges: 0 off, 1 on (the runtime's high level; the Quest only)
Option<float> VrComfortVignette("", 1.f);	// tcvr: darken the view's edges while it turns on its own: 0 off, 1 low, 2 medium, 3 high (0..1 scales low)
Option<int> VrHeadCameraLead("", -1);	// tcvr: head camera: how far ahead (ms) to aim the game camera at the head's turn; -1 measured, 0 none
Option<int> VrHeadCamera("", 2);	// tcvr: the head turns the game camera (Toy Commander): 0 off, 1 yaw, 2 yaw and pitch (default, chosen after the worn test 2026-11-18)
Option<int> VrMpLocalPlayer("", 0);	// tcvr: split screen shows only this player's view (1..4, test without netplay); 0 auto: the netplay local player; -1 off: every view, as stock
OptionString VrMpName("", "");
Option<int> VrMpMarker("", 0);
Option<bool> VrMpReturn("", false);
OptionString VrMpNames("", "");	// tcvr: the match's player names in player order, "AAA,BBB" (for "BBB left")
OptionString VrMpEndText("", "");	// tcvr: why the last match ended, for the VR menu after the restart
Option<int> VrMpTestEnd("", 0);	// tcvr: TEST ONLY (never set by the menu): at this netplay frame team 1 reaches the match limit, the game ends the match (results page); 0 off
OptionString VrMpSetup("", "");	// tcvr: the match's settings from the lobby, "mode,room,points" (the automatic setup enters them)
#ifdef USE_OPENXR
Option<int> VrUpscaleCache("", 2);
#else
Option<int> VrUpscaleCache("", 0);
#endif
OptionString GGPOPeers("", "");
Option<int> GGPOPlayer("", 0);
Option<bool> NetplayTestAnyCpu("", false);
OptionString NetplaySyncLog("", "");
Option<bool> NetplaySyncPages("", false);
OptionString NetplaySyncDump("", "");

} // namespace config
