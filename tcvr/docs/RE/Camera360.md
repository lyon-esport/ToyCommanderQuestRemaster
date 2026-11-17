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

## Found and hooked (2026-11-17)

- Camera records from `0x8C0D5904` (one per player; `0x8C03FFA8` returns the one in use). Each has two
  3x4 camera-to-world matrices, rows right, up, back, then position (y up, right-handed): `+0x154` and
  `+0x184` (`0x8C0D5A58` / `0x8C0D5A88` for player 1). The turn-in-place dumps found them directly.
- "Set camera" `0x8C0610F6` (r4 = a 3x4 camera matrix): loads it into XMTRX (`0x8C061498`), saves it at
  `0x8C0FD5BC`, inverts it into the view matrix `0x8C0FD58C`, then the culling planes (`0x8C060E20`).
- Calls seen in a mission: from `0x8C019278` (the 3D view: camera in use, matrix B), again from
  `0x8C03ED5C` and `0x8C0402B4` with the same matrix, and from `0x8C027DD6` with a copy of the camera
  elsewhere (`0x8CB8E738` in the kitchen). **The 3D is drawn through the `0x8C027DD6` call**: turning
  only the first three changed nothing on screen; turning every call turned the view.
- The hook (`core/rend/vr/xr_headcam.cpp`): BIOS trap opcode over the routine's first instruction
  (a dynarec block start, so every register is in the CPU context). For those calls, in a frame where
  the 3D view's call ran (not in menus, title, cutscenes), the camera turned by the head is written to a
  scratch matrix at `0x8C00BF00` (boot area, zero in every dump) and r4 points there. The game's own
  camera record is never written, so its chase logic and game state don't see the head.
  A first try rewrote XMTRX in the CPU context after the load: no effect (the dynarec still had the
  loaded registers in host registers).
- Each display list carries the turn its picture was drawn with (`rend_context::tcvrCamYaw/Pitch`); the
  VR view turns that picture back by as much (`gameToWorld`), so the world stays put and the head sees it
  from where it looks, with the turn matched per picture.

## Defaults and switches (VR menu > Comfort)

- **Head turns the camera** (`vr.HeadCamera`): Off / Left-right (yaw) / Left-right + up-down. Off by
  default until a wearer has checked it. Yaw is about the camera's own up axis, so in a banking plane the
  turn follows the plane's tilt; the reprojection shows it exactly either way. Pitch stays with the
  reprojection by default (the widened view already covers about +-50 degrees); roll never goes into
  the game camera.
- **Sight when looking away** (`vr.HeadCameraSight`): the weapons aim with the vehicle. The gun sight is
  a HUD sprite; whether the game projects it from the vehicle's aim (then it lands right in the turned
  view) or draws it fixed on screen is still to be checked with a head; "Hidden" is reserved for that.
- Off in netplay (GGPO, network, battle cable): the turned view matrix may feed the game's targeting.

## Testing without a head (head.request)

`echo "<yaw> <pitch> view|cam [target] [ySense pSense]" > files/head.request` (run-as): `view` turns the
eyes as if the wearer turned (shows what they'd see), `cam` turns only the game camera (the world must
not move: checks the turn back). `off` ends it. Headset tests 2026-11-17 (attract demo): with the hook on
the `0x8C027DD6` call, `view 60/180/-90` show other parts of the room; `cam 20/25` keep the world in
place with sense +1 (sense -1 shifted it by twice the angle).

## Fixes after the first real-head tests (2026-11-17)

- The vehicle stayed in front of the wearer while the room turned: it is drawn with a camera the game
  builds on its stack (look-at, the call returning to `0x8C01232A`). That call is turned too.
- Head right turned the view left: the head's yaw goes into the game camera negated (and the picture's
  turn back takes the same value, so the two still match).
