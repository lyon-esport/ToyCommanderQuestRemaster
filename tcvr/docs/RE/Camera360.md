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

## Outcome of the 2026-11-17 headset tests: parked

Manu tested it worn. Final state: the direction was right and the HUD stayed with the vehicle,
but turning was **shaky with jitter, not acceptable for play**. A 20-degree step mode
(`vr.HeadCameraStep`) showed visible jumps at each step: the camera's turn and the picture's turn
back do not match exactly. Manu asked to roll back to the stock view (the Quest got the known-good
build 20261117_003835 back) and to keep this work for later.

Measured on a still frame (mission paused, eyes fixed, `cam 30`): residual offsets of up to ~10 px
at 350 px width (about 3-4 degrees), with a roll pattern (left side down, right side up, top
left, bottom right). Most likely cause: the turn is applied about the wrong axis. The code mixes
the matrix's **rows** (treats them as the camera's axes); if the axes are its **columns**, that
turns the camera about the room's vertical instead of its own up axis, which a pitched (~10
degrees) chase camera turns into exactly that roll. Also `0x8C061F10` projects with
`sx = F0 - 604 x / z`: camera-space x points LEFT (mirrored), which is why the camera needed the
opposite sign from the picture. The last commit has a test switch for the column version
(7th number of head.request: `... 1 1 1`), not measured yet (needs a still scene: a paused mission).

Next time, in order:
1. Measure rows vs columns on a paused mission with `cam 30` (eye captures `files/capture.request`
   -> `eye0.ppm`, compare patches; the scratchpad script did 6 patches by block matching).
2. Once the residual is ~0 at 30/60/90 degrees, retest continuous follow worn; check the vehicle's
   own look-at camera (`0x8C01232A`) separately, it has its own origin.
3. Then: the gun sight (a scene sprite, not on the HUD plane), lock-on, radar, netplay.

## Rework with the decompile (2026-11-17, branch tcvr/decomp-camera360)

The function map of the decompile answered the open questions. Addresses only here (the
decompiled code stays outside the repository).

**Matrix layout (the jitter's main cause).** The load routine `0x8C061498` fills XMTRX back to
front, and the vector routines (`0x8C061BC8`, `0x8C063588`) load and store x, y, z back to front
too; the two reversals cancel. In memory order: `world = M * cam + t`, `M[row][col] = m[3*row + col]`,
`t = m[9..11]`. **The camera's axes are the matrix's columns**, not its rows. The first hook mixed the
rows, which turns the camera about the room's vertical instead of its own up axis; with the chase
camera pitched down that does not match the picture's turn back (the roll pattern measured above).
Camera space is x left, y up, z forward (`0x8C061F60`: `sx = cx - x*fx/z`, `sy = cy - y*fy/z`).
The hook now writes `M' = M * Ry(yaw) * Rx(-pitch)`: in the VR eye space (x right, y up, z back,
a half turn about y) that is exactly `Ry(yaw) * Rx(pitch)`, the turn back of `gameTurn()`.

**Which picture gets which turn (second cause).** The turn was noted at the display list's start; a
list could take the previous frame's head while its cameras had the new one (a one-frame mismatch,
as large as the head moves in a frame). Now a hook at the scene's end (kmRender/kmEndScene; two routines:
`0x8C057A86` between split-screen players, `0x8C057A5C` with one player, called by the main frame
callback through the pointer `0x8C0FC464`; the second is hooked at `0x8C057A5E`, its first
instruction being PC-relative) latches the frame's turn and the render start takes it
(`tcvrTakeFrameCamTurn` in `Renderer_if.cpp`). The head is read once per game frame, at the
simulation's camera call.

**Frame order** (main frame callback `0x8C0271F0`): simulation `0x8C01910C` (scripts, AI, physics,
then "set camera" returning to `0x8C019278`, then 3D sound), then the render callback held at
`0x8C0D21D0` (`0x8C01937C` in game, `0x8C027DBA` in menus). Per player in `0x8C01937C`: targeting
(`0x8C04041A`, sets the stock camera for the lock-on), the player's camera (`0x8C03EC88`), the world
(`0x8C0192E0`), the crosshair (`0x8C03EEF8`), the HUD (`0x8C03FDBE`), then `0x8C057A86`.

**"Set camera" callers** (return addresses), now matched one by one:

| Return | Caller | Turned |
|---|---|---|
| `0x8C019278` | end of the simulation (3D sound, culling) | yes (sound follows the head) |
| `0x8C011F18`, `0x8C01232A`, `0x8C012622` | chase cameras: car, plane (look-at), helicopter | yes |
| `0x8C03ED5C` | the player's camera | yes |
| `0x8C027DD6` | the scene camera (menu scene renderer) | yes, in a gameplay frame only |
| `0x8C03FC1C`, `0x8C03FD9E` | objective arrow's camera, and the player's put back | yes |
| `0x8C03B6BA` | turret aim (game logic) | no |
| `0x8C0402B4` | lock-on target search (game logic) | no |

Unknown callers are left alone and logged once. A gameplay frame is one where the simulation's call
ran in the last 2 VBlanks; in menus, the title and pause nothing is turned.

**HUD and sights.** The HUD plane is drawn without the turn back (it stays with the vehicle). The
crosshair, lock-on square and weapon sights (`0x8C03A824`) are the weapons' aim points (from the
vehicle) projected with the camera in use and drawn at their own depth, not on the HUD plane: with
the turned camera, the picture's turn back puts them on their point. A first try that gave them the
stock world-to-camera matrix put them at the centre of the eye at every head angle (headset,
2026-11-17), so it was dropped. The split-screen player arrows (`0x8C03F0E8`) are not handled; the
head camera is off in split screen (`0x8C0A72F4` player count > 1) and in netplay.

**Options.** VR menu > Comfort > "Head turns the camera": Off / Left-right / Left-right + up-down.
The step mode and the sight option are gone (the steps only hid the mismatch; the sight now follows
the vehicle's aim by construction).

## Headset test of the rework (2026-11-17, debug build, made-up head turns)

- First build: the picture was never turned back (`0x8C057A86` is not called with one player).
  With the `0x8C057A5E` hook: `cam 30` / `-30`: 0 px residual on 6/6 patches; `cam 60` / `90`: 0-1 px
  on the patches that still have content (before the rework: up to ~10 px). `view 180` shows the room
  behind the vehicle. The scene's end comes 1-2 VBlanks after the simulation's call (gameplay-frame
  window is 3).
- HUD gauges and radar stay toward the vehicle. VR menu (compositor layer) opens sharp; the frozen
  picture behind it does not move. VrApi 120/120 fps, no crash.
- To check worn: continuous follow, the crosshair on the aim, the menu at a turned head.
