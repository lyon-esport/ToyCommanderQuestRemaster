# Toy Commander VR: performance targets

What a build must hold on the headset before it is called good. `tcvr/docs/TestPlan-P0.md` measures
against these numbers; record each measurement with the APK version and SHA-256.

## Targets

| # | Target | Must (release) | Should | How it is measured |
|---|---|---|---|---|
| T1 | Headset frame rate, Quest 3 | **72 fps** steady in every level tried, with the default picture settings | 90 fps option later (P3) | VR menu's frame-rate line; Meta's OVR Metrics Tool (its in-headset overlay and CSV log) |
| T2 | Dropped headset frames | < 1% over a 5-minute mission (no visible stutter) | 0 in menus | logcat `XR: N black frames` lines stay empty; OVR Metrics "stale frames" |
| T3 | Game rate | **60 game frames/s** (NTSC), never PAL 50 | — | logcat `tcvr: video NTSC ... 60 vblanks/s, N game frames/s` every 10 s |
| T4 | Game speed | Real time: a mission timer runs within 2 s per minute | — | stopwatch against the in-game timer |
| T5 | Startup | Splash at once; first game picture < 20 s from the tap | < 12 s | stopwatch; logcat timestamps (process start → `loading panel` → first frame) |
| T6 | Thermal | 30 minutes of play without a Horizon OS thermal warning or throttling; CPU/GPU levels stay at "sustained high" (set by the app) | 60 minutes | OVR Metrics (temperature, CPU/GPU level); `adb shell dumpsys thermalservice` before/after |
| T7 | Battery | ≥ 1 h 45 min of play from 100% (Quest 3) | ≥ 2 h | battery % before/after a 30-minute session × 2 |
| T8 | Memory | No growth over a 30-minute session beyond the HD texture cache | — | `adb shell dumpsys meminfo com.dreamcast4ever.toycommandervr` at 1, 15 and 30 min |
| T9 | HD texture cache | Building it costs no visible stutter; size noted per level | < 300 MB for the whole game | logcat `tcvr: upscale cache`; `du -sh` of `texcache` |
| T10 | Menu | VR menu opens < 0.2 s after the press, game frozen | — | tester's note; capture |

Quest 2: not a release target until measured (backlog P1-9). Record the same table on it when one
is available and decide then.

## Picture settings budget

The defaults ship only when T1 and T6 hold with them in the busiest level tried. Each setting
below costs GPU time; measure each one on its own (VR menu, one change at a time, same spot,
note the frame rate), then the chosen combination for 30 minutes (T6).

| Setting | Default now | Expected cost | Ship if |
|---|---|---|---|
| Anti-aliasing (MSAA) | Off | 2x small, 4x moderate on Quest 3 (tile-based GPU) | 72 fps holds |
| Anisotropic filtering | Off | small | 72 fps holds |
| Sharper far textures | Off | none (may shimmer) | it looks better while moving |
| HD textures (upscale cache) | 2x | CPU while building, then texture memory | T9 holds |
| Resolution multiplier | 100% | grows with the square of the setting (130% ≈ 1.7x the pixels) | 72 fps holds with margin |
| Comfort vignette | Medium | negligible | — |

## Recording results

A table per run in the test report: date, APK version and SHA-256, level, settings, T1–T10 values,
pass/fail. Keep the OVR Metrics CSV when it was used.
