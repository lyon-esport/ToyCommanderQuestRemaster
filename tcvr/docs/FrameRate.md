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

## Still to do (plan items)

- Depth layer (`XR_KHR_composition_layer_depth`) and Application SpaceWarp (`XR_FB_space_warp`).
- A worn-headset run (inside the boundary, no passthrough) at the stock size to confirm 90 Hz and
  measure 120 Hz without the shell sharing the GPU; then choose the shipped defaults (e.g. 90 Hz,
  AF 16x, sharper High, MSAA 2x only with foveation High) and a 30-minute thermal run.
- Why MSAA 2x costs 2x here (a GPU capture with AGI/RenderDoc: look for a mid-pass resolve).
