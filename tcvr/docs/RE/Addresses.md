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
