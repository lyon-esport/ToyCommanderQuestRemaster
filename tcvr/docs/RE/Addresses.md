# Toy Commander (EU) — Reverse-engineering findings

Disc: Toy Commander (Europe) (En,Fr,De,Es). Main executable: `1ST_READ.BIN` (extract locally into `Docs/RE/dumps/`, which is git-ignored).

| Address | Size/type | Meaning | How verified | Date |
|---|---|---|---|---|
| Game ID | string | `MK-5102050` (Toy Commander EU) = `settings.content.gameId`; key for the GameProfile in core/rend/vr_reproject.cpp | logcat "Game ID is [MK-5102050]" | 2026-11-09 |

## Targets (in order)
1. Projection / FOV / culling frustum (to widen the view for VR)
2. Which polygon lists hold the HUD/2D overlays
3. Camera mode (free / fixed / cutscene) and camera and vehicle pose
4. FMV (Sofdec) playback state
5. Language and save structures
6. Unlock flags

## Session 2026-11-09 — first RAM dump (kitchen mission, `dumps/d1_kitchen`, git-ignored)
How to dump: `adb shell "run-as com.dreamcast4ever.toycommandervr sh -c 'touch files/ram.request files/capture.request'"`,
then pull `files/ram.bin` (16 MB at 0x8C000000), `vram.bin`, `eye0.ppm` (left eye), `fb0.ppm`, `scene.txt`.
Scripts: `tcvr/re/find_screen.py`, `tcvr/re/find_proj.py`, `hotd2-vr/re/sh4dis.py <start> <end> ram.bin`.

- Left-eye capture: the scene is only drawn inside the game's original 4:3 view; outside, nothing (no polygons
  sent). The black areas are therefore the game's own view culling, NOT texture streaming. Fix = widen the game's
  projection/culling (as hotd2-vr does for HOTD2/MOK).
- `scene.txt` "tan 0.5 0.375": the reprojection still uses its default vr.Focal = 640 (a guess). The real focal is unknown.
- `0x8C0A77F0..0x8C0A782C`: a 2D screen/menu record (0.5, centre 320/240 twice, clip 639/479). Written by the
  setters at `0x8C054D32` / `0x8C054D54`, read by the 2D menu hit test at `0x8C07B61A`. **Not** the 3D camera.
- `0x8C0FC490`: a renderer context passed to the library routines at `0x8C074xxx`.
- No stand-alone 4x4 perspective matrix found in RAM: the projection is probably folded into each object's
  matrix (FTRV), or done by a divide with a focal value. Next: find the 3D transform (FTRV / FSRRA / FDIV loops
  that write to the store queues at 0xE0000000) with Ghidra, or use a PC debug build (needs Visual Studio).

## Camera / projection / culling (found 2026-11-09, dump d1_kitchen)
| Address | Meaning |
|---|---|
| `0x8C0611D0` | `tan(angle)`: `lds r4,fpul; fsca fpul,dr0; fdiv fr1,fr0` (16-bit angle, 65536 = 360°) |
| `0x8C061070` | builds a view record from x, y, w, h, an angle (stack) and aspect/near/far (fr4..fr6): `+32 = 2·tan(angle>>1)`, `+28 = +32 · fr4 · w/h` |
| `0x8C061098` | `shar r4` (0x4421), the angle halving. **Patched to `nop` (0x0009)**: `tan(angle)` → about 2.5x wider; the culling follows |
| `0x8C060F20` | makes the active view: `[0x8C0FD5EC]` = record pointer; `0x8C0FD5F0/F4` = x - (+20) - 0.5, y - (+24) - 0.5; `0x8C0FD604/608` = focal = w/(+28), h/(+32); `60C/610` = 1/focal; `5F8` far, `5FC` near, `600` = 1/near; `614/618` = plane terms; culling planes at `0x8C0FD62C` (4 x 12 bytes) |
| `0x8C061F10` | projects a point: z > near, then `sx = (F0·z − 604·x)/z`, `sy = (F4·z − 608·y)/z` |
| `0x8C060740..0x8C0607A0` | culls an object: 4 planes at `0x8C0FD62C`, then a size test with the focal values |
| `0x8C0D5910` | the main 640x480 3D view record (also copies at `0x8C0C73EC`, `0x8C0C7650`): +28 = 1.63682, +32 = 0.92071 (49.4° x 78.6°), near 10, far 1e6. Used by the VR reprojection for the live focal |
| `0x8C0D21DC` | radar view, 100x90 at (470, 370), 21.97° x 32.09° |
| `0x8C0D593C` | a 128x128 view at (256, 0), near 1 (render to texture?) |

Open: whether `0x8C0D5910` is the main view in every level and in split screen; the draw-distance / LOD that
makes objects appear late (not the far plane: it is 1e6).

## HUD (dump d2_hud, kitchen, 2026-11-09)
Every polygon of a frame: `touch files/capture.request` -> `files/polys.txt` (W range, screen box, texture per polygon).
- HUD layers are flat (one W per polygon): W 1.002 (TR), 1.111 (OP+TR), 1.124 (TR: gauge needles/levels, tex 0x3d6f20).
- 3D geometry: W from ~12.9 up (near plane 10). Nothing 3D between 1 and 12.
- hotd2-vr only snapped flat layers up to W 1.05 and on 0.01 steps -> 1.111/1.124 were rebuilt as 3D right in front
  of the eyes (gauges misaligned, moving with the head). Profile: `snapMaxW = 2.0`, `snapAnyW = true`.
- Aiming marks (2D sprites; profile `reticles`, placed at the depth of the 3D under them):
  gun ring tex `0x510f40` (64 px, TR), centre dot `0x5063a0` (16 px, TR), both scaled 1/2.537 back to stock size;
  target-lock green square tex `0x6a91f0` (OP, 16-vertex outline, ~22 px, W 1.000, colour 00ff00), scale 1.
- Lock square: its texture moves between loads (`0x6a91f0`, then `0xacafa8`): matched by colour 00ff00ff + 16 verts.
  Capture d7_now: in the game's own frame the square is exactly on the locked mouse (square 296..320 x 226..250,
  mouse polys 293..315 x 235..241). Left on the 2D plane (stock framing) it lands off target -> it must go into 3D.
- `[0x8C0FD5EC]` = pointer to the view record in use (set by 0x8C060F20). In menus (after a mission) it points to
  `0x8C0D21DC`, a 640x480 view with focal ~595 (dump d11_menu_after); in missions the radar view is set last.
  The VR focal now comes from this record when it is full screen, else from 0x8C0D5910 (mission view).

## Decompile review 2026-11-17 (Ghidra, static; addresses only)

| Address | Meaning |
|---|---|
| `0x8C02796A` | game main(), called by the Shinobi start-up `0x8C0B0340`; per-frame callback `0x8C0271F0` run by the loop in pointer `0x8C0A76A0` |
| `0x8C0610F6` / `0x8C0610DA` | set / commit the camera matrix: camera-to-world `0x8C0FD5BC` (position `0x8C0FD5E0`), world-to-camera `0x8C0FD58C`, then frustum planes (`0x8C060E20` -> `0x8C0FD62C`). Single choke point for a head-driven camera |
| `0x8C03EC88` | per-player camera update: chase cameras `0x8C0117F4` (car), `0x8C011F6C` (plane), `0x8C012378` (heli) |
| `0x8C0D5904` | 4 player records x 0x26C (P1 main view record `0x8C0D5910` = +0x0C) |
| `0x8C0FD7C8` | 4 pad slots x 0x30 (+0 held, +4 pressed, +0x0C/+0x10 stick, +0x18/+0x1C triggers); input enters the simulation at `0x8C01BA36` |
| `0x8C0A76C4` | game clock in ms; `0x8C068220` rand, seed `0x8C0AF748` (reseeded at boot and at each scene load) |
| `0x8C040BF6` | demo playback: injects recorded pad values per frame (netplay reference) |
| `0x8C01E97C` | mission-script statement loop; handler tables `0x8C0A57D4` (statements), `0x8C0A5614` / `0x8C0A5598` (int / float expressions, token - 163) |
| `0x8C0C7DC4` | mission status, u16 per room, 2 bits per mission (0 locked, 1 open, 2 done, 3 done+); `0x8C0A5A5C` rooms mask; rules `0x8C02A02C` |
| `0x8C0C902C` | mission table, 0x50 per entry (index room x 8 + mission - 1): +0x20 best time ms, +0x24 records, +0x38 names |
| `0x8C033238` | VMU job runner; system file `TOY_COMM.___` (1,408 bytes, `0x8C03280E` pack / `0x8C032AB6` load), saved game `TOY_COMM.<name>` (384 bytes, `0x8C032DD4` / `0x8C033026`); no checksum, VMS CRC 0 |
| `0x8C018F42` | applies the screen-format option `0x8C0A5FF8` (aspect `0x8C0A4120`) |
| `0x8C030CF4` | menu page dispatch from request bits `0x8C0A5A6C` (0x100 options `0x8C02B238`, 0x800 screen size/borders `0x8C028114`) |
| `0x8C07B614` | correction: the "menu hit test" `0x8C07B61A` belongs to a developer window toolkit, not the game menus |

The "2D menu hit test" `0x8C07B61A` listed above is inside `0x8C07B614`, the hit test of a developer
window toolkit (debug file browser); the game menus are Menu.ghq scripts plus the pages dispatched by
`0x8C030CF4` (options `0x8C02B238`, screen size / borders `0x8C028114`).

Save layout and "Unlock all levels": see `../Saves.md`. Mission end `0x8C022E04` (jumped to from the frame
callback when byte `0x8C0A4130` is set) writes the best time and raises the status to 3 when the time beats
the mission table's target (+0x4C), else to 2, then runs the unlock rules `0x8C02A02C`. Cheat matcher
`0x8C0144DA` (17 six-button codes at `0x8C0A3F08`); code 5 (`0x8C02259A`) opens every mission.

## Multiplayer menus, scores and split-screen sound (2026-11-18, static; addresses only)

| Address | Meaning |
|---|---|
| `0x8C0A5A6C` | menu request bits: 0x4000 team/vehicle setup `0x8C02CB48`, 0x10000 multiplayer options `0x8C02D368`, 0x40000 cat message `0x8C02E340`, 0x80000000 end-of-match results `0x8C02FAD8`; input for all of them in `0x8C031DE0` |
| `0x8C0A6048` | player slots, 4 x 0x28: +0 team 0..3, +4 vehicle 0..26 (models `0x8C0CD418`, 0x4C each), +8 state (0 no pad, 1 pad present, 2 choosing, 3 ready) |
| `0x8C0A72D4` | menu player record (the "MASTER" quarter) |
| `0x8C0A5AD0` | options: room, index into the unlocked arenas (table `0x8C0CBE48`, 0x14 each, rooms mask `0x8C0A5A5C`); with every room open 0..7 = Kitchen and dining room, Two children's bedrooms, Hallway and its mezzanine, Parent's bedroom, Garage and tool room, Attic, Living room and winter garden, Cellar |
| `0x8C0A5ACC` | options: mode (0 Deathmatch, 1 Cat and Mice (time limit), 2 Capture the Flag) |
| `0x8C0A5AC8` | options: "Active Environment" On / Off |
| `0x8C0A60E8` / `0x8C0A60EC` | options: limit index 0..21, live while the page is open and kept when it is left with B (0 no limit, then 1..10, 15, 20, 25, 30, 40..100 points; in mode 1 30, 60, 90, 120, 180 .. 1140 s; default 10) / limit value, set when the page is confirmed |
| `0x8C0A5A78` | options page cursor line: 0 Room, 1 Mode, 2 Active Environment, 3 limit (starts on 0 each time the page opens) |
| `0x8C0A5A6C` bit `0x10000000` | "Only one team has been selected... continue?" (A yes, B no), before the options page when one team is ready. The word reads differently mid-frame (bits come and go): read it at the end of a frame |
| `0x8C0A5AD4` | cat game: team holding the cat (8 = none) |
| `0x8C0C7DD4` | team scores, 4 x u32 by team (cleared when the options page is confirmed) |
| `0x8C0CDC68` | kill table, 4 rows x 0x14 (killer x victim, column 4 team points) |
| `0x8C0253F8` / `0x8C0248E0` | menu script system commands (slot reset 0x0D, leave multiplayer 0x42, "team n reached the limit" 0x47..0x4A) |
| `0x8C024080` | script statement 151: writes team scores, cat team and kill table |
| `0x8C04BE04` | split-screen sound update: listeners `0x8C0D58F4` (player +0x184 matrices), count `0x8C0A72DC`; sources 128 x 0x9C at `0x8C0DA3B4` keep per-listener distance (+0x14 + 4i) and position (+0x3C + 0x0Ci) |
| `0x8C04AED2` | sound random pitch uses the game rand `0x8C068220`: per-headset sound changes inside the game would desync netplay |

## One-player room select page (2026-11-22, live on PC; the arena picture tour, `core/network/tcvr_arena_pics.cpp`)

| Address | Meaning |
|---|---|
| `0x8C0A5A6C` | menu request word: `0x00200023` title, `0x0002002x` main menu (bit `0x20000`, "One player" picked first), bit `0x40` room select page (`0x63`) |
| `0x8C0A5A78` | room select page cursor = room 0..7 (the arenas' order); writing it changes the room the next frame |
| `0x8C0A6040` | the previous cursor (the page notices a change by comparing both) |
| `0x8C0A603C` | the house's turn offset (eased, plus a slow idle turn); 0 written every frame holds each room's own angle |
| `0x8C0A5A5C` | rooms unlocked (u16); `0x00FF` shows every name and stills the house; a locked room shows "Coming Soon!" and a pulsing red highlight |

Route with no memory card in either slot (no save written, no name or profile page): START every 1.5 s from 2 s
(logos, title, "insert a memory card" warning), START once the main menu has been there 0.6 s, room page at
about 11.7 s of game time. A room's view is still 0.4 s after the cursor write.
