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
How to dump: `adb shell "run-as com.flycast.emulator.vr sh -c 'touch files/ram.request files/capture.request'"`,
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
