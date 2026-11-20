# Toy Commander VR: headset frame rate (72 / 90 / 120 Hz)

Measurements and changes from the frame-rate work of 2026-11-16 (Quest 3, kitchen training
mission, plane parked on the runway, nobody wearing the headset). The game itself always runs at
60 vblanks/s and draws 30 pictures/s; this is about the headset's frames.

## How to measure (read this first)

- VrApi prints one `FPS=` line per second **per VR process**. With the system shell
  (`com.oculus.vrshell`) also rendering, half the lines are the shell's, not the game's: filter
  by the game's PID (`adb logcat -s VrApi:I` and keep lines whose PID column is
  `pidof com.dreamcast4ever.toycommandervr`). Earlier 90/120 Hz numbers that mixed both are wrong.
- Check the developer overrides before measuring: `adb shell getprop | grep debug.oculus`.
  `debug.oculus.refreshRate` pins the display rate; `debug.oculus.textureWidth/Height` replace the
  runtime's recommended eye-buffer size (a test headset had 2800x2933, about 1.84x the pixels of
  the stock Quest 3 size), so "100%" is not what players get. `debug.oculus.gpuLevel` pins the
  GPU clock level. To simulate the stock size without touching the properties, set
  `vr.XrResolution` so that width x scale is about 2064 (0.74 for 2800).
- An unworn headset that sits outside its boundary shows passthrough over the game, and the shell
  shares the GPU: compare builds A/B in the same state rather than reading absolute numbers.

## Results (VrApi, game process only; App = app GPU time per frame)

| Eye buffer | Settings | Display | Before | After |
|---|---|---|---|---|
| 2800x2933 (developer override) | defaults | 72 Hz | 72.3 fps, App 9.3 ms | 72.4 fps, App 9.0 ms |
| 2800x2933 | defaults | 90 Hz | 75.8 fps, 20 stale/s | 74.8 fps (GPU-bound) |
| 3080x3226 (110%) | MSAA 2x, AF 16x, sharper High | 72 Hz | 34.8 fps, App 23.3 ms | 37.9 fps, App 21.3 ms (MSAA 2x, no AF/LOD) |
| 2072x2170 (stock size) | defaults | 90 Hz | — | **90.5 fps**, 0.6 stale/s, App 5.2 ms |
| 2072x2170 | AF 16x, sharper High | 72 / 90 / 120 Hz | — | 72.5 / **90.5** / 107.5 fps |
| 2072x2170 | MSAA 2x, AF 16x, sharper High, foveation off | 90 Hz | — | 61.1 fps, App 11.7 ms |
| 2072x2170 | same, foveation Medium | 90 Hz | — | 85.2 fps, App 7.8 ms |
| 2072x2170 | same, foveation High | 90 Hz | — | **90.4 fps**, 3 stale/s, App 6.6 ms |

Findings:
- The limit is the GPU, not the render thread (render thread 30–55% of one core at 90–120 Hz).
- MSAA 2x about doubles the app GPU time on this path (5.7 -> 11.6 ms at stock size); fixed
  foveation High wins most of it back.
- At the stock eye size 90 Hz holds with AF 16x and sharper textures; with MSAA 2x it needs
  foveation (Medium is not enough at 90 Hz).

## What changed

1. **Kept frame uploaded once per game picture** (`core/rend/gles/gles.cpp`): the vertex, index
   and modifier-volume buffers of the kept game frame go to the GPU when a new picture arrives,
   not again for every eye of every headset frame. Neutral in the measurements (the render thread
   was not the limit) but removes 2–8 full-buffer rewrites per picture.
2. **Depth/stencil discarded after each eye** (`core/rend/vr/xr_host.cpp`,
   `glInvalidateFramebuffer`): the projection layer is colour only, so the tile GPU no longer
   writes depth and stencil (every MSAA sample of them) out to memory. About -2 ms with MSAA 2x
   at 110%. (A depth layer for SpaceWarp would need depth stored again: revisit then.)
3. **Fixed foveated rendering** (`XR_FB_foveation`, `XR_FB_foveation_configuration`,
   `XR_FB_swapchain_update_state`): option `vr.Foveation` 0–3, VR menu line
   "Fixed foveation (edges)" Off/Low/Medium/High, applied live and again to new eye buffers
   (MSAA or resolution change). Default Off.

Items 1 and 2 can be switched off to compare or to rule them out of a bug: `vr.UploadOnce` and
`vr.DiscardDepth` (both on by default), VR menu lines "Upload frame once (perf)" and
"Discard eye depth (perf)", after the foveation line; both take effect on the next headset frame.

## Still to do (plan items)

- Game pictures: done 2026-11-18, see "Game pictures in missions: 60/s" below (headset test pending).
- Measured 2026-11-17 (worn): at 100% the GPU is 65–84% busy in 3D levels with 0–6 stale
  frames/s at 120 Hz; at 130% it is saturated in the heaviest views (see Defaults.md).

- Depth layer (`XR_KHR_composition_layer_depth`) and Application SpaceWarp (`XR_FB_space_warp`).
- A worn-headset run (inside the boundary, no passthrough) at the stock size to confirm 90 Hz and
  measure 120 Hz without the shell sharing the GPU; then choose the shipped defaults (e.g. 90 Hz,
  AF 16x, sharper High, MSAA 2x only with foveation High) and a 30-minute thermal run.
- Why MSAA 2x costs 2x here (a GPU capture with AGI/RenderDoc: look for a mid-pass resolve).

## Game pictures in missions: 60/s (option `vr.Game60`, default on)

Stock Toy Commander draws 60 pictures/s in menus and 30 in missions: a mission load and leaving
the pause menu call Kamui's "wait vsync count" with 2, menus and pause with 1. Kamui keeps
vblanks - 1 at 0x8C10FC80 and copies it into every picture's flip. The game's clock, physics
(8 ms substeps), particles, fades and mission timer follow real time, so a mission keeps its
speed with 1 vblank per picture and only gets smoother (less judder in the reprojected view).

`core/hw/pvr/spg.cpp` checks that value once per vblank and turns 1 into 0 (no change to the
game's code). The attract demos replay one recorded frame per picture, so they keep 30 (demo mode
0x8C0A72FC = 2). VR menu > Performance > "Game pictures in missions": "30/s (original)" or "60/s";
turning it off takes effect at the next mission or pause. Logcat line when it applies:
`tcvr: mission at 60 pictures/s`; the existing `tcvr: video ... game frames/s` line shows the rate.

The emulated SH-4 must finish a mission picture in 16.7 ms. At the stock 200 MHz it doesn't, and
the rate settles at an uneven 37.5/s (a 60/30 mix). With the option on, `core/emulator.cpp`
(`tcvrApplyGameRate`) raises `Sh4Clock` to 360 MHz for this game (not saved; the player's value
comes back when the option is turned off; never online). Headset test 2026-11-18, Kitchen
Training, 130% + MSAA 2x, head camera on:

| Setting | Game pictures/s | Emulator thread (one core) | Head camera lag | Mission clock / wall |
|---|---|---|---|---|
| option off, 200 MHz | 30.0 | 45% | 14.4 headset frames | - |
| on, 200 MHz | 37.5 (60/30 mix) | 49% | 5.7 | 60.06 / 60 s |
| on, 250 MHz | 47.4 | 56% | 5.0 | - |
| on, 300 MHz | 56-58 | 63% | 4.5 | 59.73 / 59.7 s |
| on, 360 MHz | 59.95, also flying | 61% | 7.4 | 59.60 / 59.58 s |

Some rooms are too heavy for the Quest's CPU at 360 MHz and 60 pictures/s: in the children's
bedrooms the game ran at 51 vblanks/s, its clock at 85% of real time. So `spg.cpp` watches the
vblanks per real second: under 57 for 2 s (not judged in the first 8 s after the game sets its
rate, the mission's loading), the mission goes back to 30 pictures/s and the SH-4 to its own clock
(at 360 MHz and 30 pictures/s the bedrooms still ran at ~98%; at 200 MHz exactly real time). 60 is
tried again after the next pause or mission. Logcat: `tcvr: emulation behind real time ...`.
Kitchen Training never falls back (80 s, pause/resume).

Headset frame rate is unchanged by the option (it depends on resolution and MSAA). Still to
watch: physics feel at smaller steps, audio and cut-scenes at 360 MHz, the other rooms.
