# Roadmap

What's next for Toy Commander VR, in order. Priorities use MoSCoW: **P0** Must (first
distributable test build), **P1** Should, **P2** Could, **P3** Won't have now.
Last reorganized 2026-11-16. The House of the Dead 2 VR roadmap this fork started from is
kept in [ROADMAP.hotd2-vr.md](ROADMAP.hotd2-vr.md).

## Decisions so far

- **VR menu:** drawn by the emulator as an overlay on top of the game, pausing it. The game's own menus are not hijacked.
- **Aiming:** the weapons aim with the vehicle, as in the original game. The head only
  turns the view and never feeds the game's aiming.
- **360° head camera:** moved out of the first release (2026-11-17). It will come in a later
  update, built on the decompile branch (see below).
- **PAL only:** the memory-patch profile targets the European release (`MK-5102050`).

Still open: whether pitch and roll follow the head in planes, where the gun sight is drawn
when the player looks away from the vehicle's heading, and the public name and icon (see P0-7).

## Done

- One APK with no game data. The disc image is found in Download, and an in-headset setup
  panel asks for file access.
- Stereo VR at 72 fps with head tracking (3DOF + 6DOF). Recenter: right stick click.
- Meta Touch controllers act as a Dreamcast pad (see [Controls](README.md#controls)).
- Saves: automatic backup at launch, export / import, and import through `import.request`
  (see [tcvr/docs/Saves.md](tcvr/docs/Saves.md)).
- English by default. Version names are `YYYYMMDD_HHMMSS`.
- Wider game view (FOV and culling patch), HUD on the 2D plane.
- 3D aiming marks (gun ring, lock square, bomber mark).
- Audio stall and speed fix (validated on the headset, being pushed).

## Phase 1 – P0: playable, distributable test build

| # | Item |
|---|---|
| P0-1 | **PAL version check at launch:** accept only `MK-5102050` (plus a file hash if possible), with a clear message on the setup panel otherwise. |
| P0-2 | **60 Hz for the PAL game:** check whether it runs at 50 Hz, and force 60 Hz if it can. |
| P0-3 | **VR menu overlay** on a long press or a double press of the left ≡ button, pausing the game: recenter, world size, export / import saves, quit at the bottom. A short press of left ≡ is Start (the game's menu, and the only Start); the right stick click recenters. |
| P0-4 | **End-to-end save test:** play, save, quit, relaunch, load, export, import. |
| P0-5 | **Control model written down:** vehicle aiming (decided); pitch / roll in planes and sight placement still to define. |
| P0-6 | **Release readiness:** release keystore and signing, sideloading (SideQuest / `adb`), an `Install.md` for players, and the GPL source published with each APK. |
| P0-7 | **Public name and icon** that avoid the "Toy Commander" trademark and Sega's logo. Needed before any public release; private test builds are not affected. **Undecided.** |
| P0-8 | **360° view:** moved to the [decompile branch](#decompile-branch-tcvrdecomp) on 2026-11-17 (not in the first release). |
| P0-9 | **Basic comfort:** vignette when the view turns on its own, recenter in the VR menu. The 360° part moves with P0-8. |

## Phase 2 – P1: should have

| # | Item |
|---|---|
| P1-3 | Other comfort options: stable horizon when planes bank, world scale in the menu, a clear "snap rotation" for vehicles (the view offset, not the vehicle). |
| P1-4 | Fixed-camera missions and cutscenes keep their own view; the intro video plays on a flat screen or can be skipped. |
| P1-5 | Xbox Bluetooth controller, with automatic switching between it and the Touch controllers. |
| P1-6 | Quit to Meta Home at the bottom of the VR menu (built with P0-3). |
| P1-7 | Sharper image: 4x MSAA, anisotropic filtering, negative LOD bias, slight supersampling. |
| P1-8 | Launch feedback: system splash and a loading panel (searching for the game, checking the version, starting). |
| P1-9 | Performance targets: 72 Hz minimum on Quest 3, a decision on Quest 2, CPU and thermal budget. |
| P1-10 | Touch controller mapping documented for players and shown in the VR menu. |
| P1-11 | Audio follows the head once the camera turns; decide on spatial audio. |

## Phase 3 – P2: could have

| # | Item |
|---|---|
| P2-1 | Graphics presets in the VR menu (Performance / Balanced / Quality / Custom), fixed foveated rendering. |
| P2-2 | Smooth camera turn following the vehicle. |
| P2-3 | Cockpit view and a farther 3rd-person view. |
| P2-4 | Head-locked HUD as an option. |
| P2-5 | LAN multiplayer, up to 4 headsets: each runs the 4-way split screen, only inputs are exchanged, each headset shows its own player. Gates: head orientation in netplay inputs if it changes game state, split-screen detail acceptable, CPU budget for 4 views. Rollback (GGPO) or lockstep to be decided. |
| P2-6 | All original languages (Fr / De / Es). |
| P2-7 | Unlock all levels. |
| P2-8 | Update check from GitHub Releases in the VR menu. |
| P2-9 | Texture packs from a folder, real-time xBRZ as a first test. |

## Decompile branch (`tcvr/decomp`)

Started 2026-11-17, after v1.0.0. The game is decompiled so that features can use its real code
instead of patches found by RAM dumps. Work goes on the long-lived branch `tcvr/decomp` and
reaches `main` feature by feature. Workflow and copyright rules (no game code or data in this
repository): [tcvr/docs/Decomp.md](tcvr/docs/Decomp.md).

| # | Item | Parked where |
|---|---|---|
| D-1 | **Decompile workspace:** extract `1ST_READ.BIN`, set up the disassembler project, name the routines already known (`tcvr/docs/RE/Addresses.md`). | – |
| D-2 | **360° head camera** (was P0-8): head yaw (and pitch in planes) turns the game camera, the HUD stays with the vehicle, the sight and lock-on behave when looking away. | `tcvr/camera-360`, draft PR #18, [Camera360.md](tcvr/docs/RE/Camera360.md) |
| D-3 | **Nicer water:** find the water textures and whether the game animates them, then a water-only shader (ripples, fresnel, glint) with an Original / Enhanced switch. | Notes of 2026-11-17 |
| D-4 | **VR options in the game's own menus:** needs the menu action handlers that the RAM-dump search did not find (stopped 2026-11-16). | `find_menu.py` notes |
| D-5 | **Multiplayer groundwork for P2-5:** which game state the head orientation and the turned camera touch (targeting, AI, culling), determinism of a frame, and what the netplay inputs must carry. | Open question in P2-5 |
| D-6 | Later: horizon lock in planes (P1-3), fixed-camera missions (P1-4), cockpit view (P2-3), level unlock (P2-7). | – |

## P3: not now

- Online play over the Internet with room codes.
- On-device AI texture upscaling (a PC companion tool is the fallback).
- Normal and relief maps generated from the textures.
- Gyro and accelerometer piloting.
- Refresh rate selector (cheap, could join P2-1).
- VR menu in other languages.
- PC VR build of Toy Commander (possible, inherited from HOTD2 VR; the Windows PCVR CI
  already builds the emulator).
- Static recompilation as a "true remaster".

## Suggested order

1. Push the validated audio fixes.
2. P0-1 and P0-2, then P0-3 (with P1-6), then P0-4.
3. After v1.0.0: decompile-era work (D-1, then D-2) on `tcvr/decomp`, v1.0.x fixes on `main`.
4. P1-7, P1-8 and P1-9 in parallel with the camera work.
5. P0-6 and P0-7 before the first APK leaves the team.

No dates: this is a hobby project.
