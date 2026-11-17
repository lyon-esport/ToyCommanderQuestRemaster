# 360° head-driven camera: reverse-engineering plan (backlog P0-8)

Goal: the player turns their head and sees all around the vehicle, while the vehicle keeps
steering and aiming on its own (decision of 2026-11-16: the weapons aim with the vehicle, the
head only turns the view).

## Why the reprojection alone can't do it

The headset already re-projects the game's 3D for each eye, so small head turns look right.
But the game only draws and culls what is inside its own camera's view (widened to about 2.5x by
the FOV patch at `0x8C061098`). Behind the player there is nothing to show. The game's camera
itself has to turn with the head, so that the game draws what the head looks at.

## What is known (see Addresses.md)

- The main 3D view record `0x8C0D5910` (size, `2·tan(half angle)`, near, far) and the active-view
  pointer `[0x8C0FD5EC]`, set by `0x8C060F20`, which also writes the focal values and the four
  culling planes at `0x8C0FD62C`.
- Points are projected by `0x8C061F10` (`sx = (F0·z − 604·x)/z`): x, y, z are already in the
  camera's frame by then. So a world-to-camera transform runs before it, probably with FTRV
  (`tcvr/re/find_transform.py` lists FTRV + reciprocal windows).
- Not known yet: where the camera's position and orientation live, and the code that builds the
  view matrix from the vehicle's pose each frame (the chase camera).

## Method

1. **Series of dumps while turning.** In a mission with the chase camera, take numbered RAM dumps
   while the vehicle turns in place, with nobody at the controllers:

   ```
   tcvr/tools/ramdump-series.sh turn_kitchen 6 700 1
   ```

   It writes `ram_00.bin` ... `ram_05.bin` (+ the head direction in `ram_NN.txt`) into
   `tcvr/docs/RE/dumps/turn_kitchen/` (git-ignored), steering right for 0.7 s between dumps with
   scripted pad input (`tcvr/tools/pad.sh`, the `files/pad.request` test aid in `xr_host.cpp`).
   Take a second series turning left, and one with the vehicle still and the head turned (to
   tell the camera from the head: nothing in game RAM should follow the head yet).

2. **Find the rotation.** `python tcvr/re/find_camera.py dumps/turn_kitchen/ram_*.bin` lists the
   3x3 rotation matrices (3x3, 3x4 or 4x4 float layouts) whose yaw changes in every step, the
   level ones first, and the 16-bit angle words (65536 = 360°) that follow the best one. The
   chase camera's matrix, the vehicle's matrix and their angle words should all turn by about the
   same amount; the camera's is level (pitch small) and is read by the code before
   `0x8C061F10`.

3. **Confirm by writing.** With the address in hand, poke a yaw offset into it from the profile
   (`GameProfile::pokes` in `core/rend/vr_reproject.cpp` writes words every frame) and look:
   - If the view turns and the vehicle keeps going straight, it is the camera (or its yaw).
   - If the vehicle turns too, it is the vehicle's heading: go one step later in the code.
   Prefer a per-frame write of the camera's yaw (offset by `vr::xr::headYawPitch().x`) after the
   game computes it and before `0x8C060F20` builds the view and culling planes, so culling follows.
   If no data write works (the camera is rebuilt from the vehicle every frame), patch the code that
   builds it (a code hook: find its caller with Ghidra in `tcvr/docs/RE/dumps/ghidra`).

4. **Then the reprojection.** Once the game camera turns with the head, the reprojection must stop
   turning the picture by the same yaw (otherwise the turn is doubled): the eye view uses the head's
   rotation relative to the game camera's new direction. Pitch stays with the reprojection at
   first (open question: does pitch/roll follow the head in planes?).

## First run on old dumps (2026-11-16)

`find_camera.py` over the six dumps `d6_1..d6_6` (taken during play on 2026-11-09, not a
controlled turn): 142 rotation matrices change yaw in every step, in 20 s. Most are level
(pitch 0) and spin through large angles: spinning pickups and props, at `0x8CAB....-0x8CAD....`
in 3x4 layouts (stride 12, often two copies 0x34 bytes apart). One stands out as a candidate for
a chase camera: `0x8CAB7944` (stride 12) is the only one pitched (12 to 21 degrees, looking down).
To check with a controlled series: it should turn by the steering's amount, and only then.

## Plumbing already in place (prep, 2026-11-16)

- `vr::xr::headYawPitch()` (`core/rend/vr/xr_host.h`): the head's yaw and pitch relative to the
  recenter direction, updated every headset frame, readable from the emulator thread.
- `vr::xr::setArtificialTurnRate()` (`core/rend/vr/xr_vignette.h`, comfort vignette branch): feed it
  the injected camera's turn rate that does NOT come from the head (vehicle turning) so the vignette
  follows the real camera.
- Named RAM dumps: `echo NAME > files/ram.request` writes `ram_NAME.bin`, `vram_NAME.bin` and
  `ram_NAME.txt`.
- Scripted pad input: `echo "x y BUTTONS ms" > files/pad.request` (`tcvr/tools/pad.sh`).

## Open questions

- Pitch and roll in planes: head or vehicle?
- Where the gun sight is drawn when the head looks away from the vehicle's heading (it aims with the
  vehicle: probably a 3D mark on the vehicle's axis, the game's own 2D one hidden or moved).
- Split screen (multiplayer): one camera per player; the same record layout is expected, to check.
