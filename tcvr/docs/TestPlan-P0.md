# Headset test plan: P0 batch of 2026-11-16

Covers the four P0 changes made without the headset, each in its own PR:

| Area | Backlog | Branch |
|---|---|---|
| A. Only Toy Commander (Europe) runs; the setup panel says why | P0-1 | `tcvr/pal-disc-check` |
| B. 60 Hz for the European disc, video-rate log | P0-2 | `tcvr/force-60hz` |
| C. Comfort vignette while steering | P0-9 | `tcvr/comfort-vignette` |
| D. 360° camera prep: scripted pad input, dump series, head yaw | P0-8 | `tcvr/camera-360-prep` |
| E. Image sharpness: LOD bias, MSAA and eye resolution switched live | P1-7 | `tcvr/sharpness` |
| F. VR menu on the left ≡ (overlay), with a toggle per sharpness setting | P0-3, P1-6 | `tcvr/vr-menu` (on top of E and C) |
| G. Launch splash and loading panel | P1-8 | `tcvr/launch-feedback` |
| H. HD texture upscale cache (built once from the player's disc) | P2-9 | `tcvr/texture-upscale` (on top of F) |

Test build: the local branch `tcvr/test-p0` = `main` + the audio fixes (PR #2) + all the branches above.
Record the APK's version and SHA-256 with each result. A test passes only with the evidence listed.

## Setup (laptop, Git Bash, from `code/`)

```bash
source tcvr/tools/env.sh
bash tcvr/tools/connect.sh            # prints export ANDROID_SERIAL=...; run that line
"$ADB" shell am broadcast -a com.oculus.vrpowermanager.prox_close   # keep running when not worn (unattended tests only)
bash tcvr/tools/deploy.sh shell/android-studio/flycast/build/outputs/apk/vr/flycast-vr.apk
"$ADB" logcat -c
```

Logs: `"$ADB" logcat -s flycast` (native and Java). Screen: `bash tcvr/tools/screenshot.sh` (system
compositor view) or, in game, `"$ADB" shell run-as com.dreamcast4ever.toycommandervr touch files/capture.request`
then pull `files/eye0.ppm` (left eye as the game draws it).
After an unattended session: `"$ADB" shell am broadcast -a com.oculus.vrpowermanager.prox_open`.

Before anything: note the device config and saves, so they can be checked unchanged afterwards:
`"$ADB" exec-out run-as com.dreamcast4ever.toycommandervr cat files/emu.cfg | sha256sum` (path as found by
`run-as ... ls -R files | head`), and the VMU files' SHA-256 (see `tcvr/docs/Saves.md`).

## A. PAL disc check

| # | Steps | Expected | Evidence |
|---|---|---|---|
| A1 | Only the European disc image in Download/ToyCommanderVR. Launch the app from the library. | The game boots as before (intro, then menus). | logcat `tcvr: supported disc [MK-5102050] version [V1.xxx]`; `Game ID is [MK-5102050]` |
| A2 | Add another Dreamcast disc image as .gdi or .cue (any other game, or another Toy Commander region) next to it, in its own folder. Open the setup panel (launch the app: two images found → the panel). | The other image is listed in red: "Found …, but it isn't the European Toy Commander disc (its game ID is …)", and has no Play button. The European one keeps its Play button. | screenshot of the panel |
| A3 | Only another disc as .chd or .cdi (move the European one away). Launch the app. | The game doesn't start: the setup panel comes back within a few seconds, with the red message for that file. No black headset that stays. | logcat `tcvr: unsupported disc [...]` (ERROR) then `tcvr: unsupported disc ... (game ID [...])` (Java); screenshot |
| A4 | Launch again with the same .chd still there. | The panel shows it as the wrong version at once (remembered), the game process isn't started. | logcat: no `Game ID is` line |
| A5 | Put the European image back, remove the other. Launch. | Boots the game (A1). | as A1 |

## B. 60 Hz

| # | Steps | Expected | Evidence |
|---|---|---|---|
| B1 | Boot the game, wait 30 s in the menus, then start a mission and play 30 s. | Every 10 s a line `tcvr: video NTSC ... ~60 vblanks/s` (or `VGA ... 480p ~60`), never `PAL ... 50`. Game frames/s ~60 in a mission (30 is a game that draws every other frame: note it). | logcat lines `tcvr: video` and `tcvr: forcing NTSC broadcasting (60 Hz)` |
| B2 | Speed and sound in a mission (worn headset). | Music pitch and game speed normal; a mission timer runs at real time (stopwatch over 60 s, within 2 s). | tester's note, stopwatch values |
| B3 | Comparison (optional): launch with `--es hotd2vr.config "vr.Force60Hz=no"` (adb `am start` on the launcher activity). | `tcvr: video PAL ... ~50 vblanks/s`; visible judder; game slower or the same per second (note which). | logcat lines |
| B4 | Language and region still right (English menus, European content). | Same as before the change. | screenshot |

## C. Comfort vignette

| # | Steps | Expected | Evidence |
|---|---|---|---|
| C1 | In a mission, hold the left stick fully left for 2 s, then release. | The edges of the view darken quickly (within ~0.1 s), the clear area shrinks to about 30° around where you look; after release it fades within ~0.5 s. Head-locked: the dark ring stays with the head. | `capture.request` eye image while steering (`pad.sh - 2000 -1 0` then capture) shows the dark ring; tester's note |
| C2 | Small steering corrections. | A light vignette, no flicker. | tester's note |
| C3 | In menus (mission select, briefing), move the left stick. | No vignette. | tester's note / capture |
| C4 | Launch with `--es hotd2vr.config "vr.ComfortVignette=0"`. | No vignette at all. | capture while steering |
| C5 | Comfort: two testers rate a 2-minute mission with and without (0 / 0.6). | Not worse with it; record each tester's comfort 1-5. | notes |
| C6 | Performance: watch for dropped frames while steering. | No new stutter; `XR: N black frames` lines don't increase. | logcat |

## D. 360° camera prep

| # | Steps | Expected | Evidence |
|---|---|---|---|
| D1 | In a mission: `bash tcvr/tools/pad.sh START` | The game pauses (its pause menu); again: resumes. | logcat `XR: scripted pad input`; screenshot |
| D2 | `bash tcvr/tools/pad.sh - 2000 1 0` | The vehicle turns right for 2 s (and the vignette shows, C1). | capture before/after |
| D3 | `bash tcvr/tools/ramdump-series.sh turn_right 6 700 1` in a mission with the chase camera, vehicle on open ground. | Six dumps of 16 MB each with `ram_NN.txt` (game, head yaw/pitch). | file list printed by the script |
| D4 | The same with steer `-1` (turn_left) and with steer 0 while the tester turns their head 90° between dumps (head_only). | As D3. | as D3 |
| D5 | `python tcvr/re/find_camera.py tcvr/docs/RE/dumps/turn_right/ram_*.bin` (and turn_left, head_only) | Candidate matrices whose yaw changes by the steering's amount in turn_right/turn_left and NOT in head_only. Check `0x8CAB7944` (pitched candidate from the old dumps). | output saved next to the dumps; findings added to `tcvr/docs/RE/Camera360.md` and `Addresses.md` |
| D6 | Recenter (left stick click) facing another way, then a dump: `ram_NN.txt` head yaw ~0 right after recenter. | Head yaw is relative to the recenter direction. | `ram_NN.txt` |

## E + F. VR menu and image sharpness

Scripted menu input (nobody at the controllers): `"$ADB" shell "run-as com.dreamcast4ever.toycommandervr sh -c 'echo toggle > files/menu.request'"`
(words: toggle, up, down, left, right, accept, back; one per request, 0.1 s apart at least).
Capture the menu with `files/capture.request` (left eye) or a screenshot.

| # | Steps | Expected | Evidence |
|---|---|---|---|
| F1 | In a mission, press the left ≡ (or `toggle`). | The menu appears ~1.6 m ahead where the head looks, over the frozen game; the game is paused (sound stops, nothing moves); head tracking still smooth. Lines: Resume, Recenter view, World size, Game behind the menu, Comfort vignette, Anti-aliasing (MSAA), Anisotropic filtering, Sharper far textures, Eye resolution, Export saves, Import saves, Quit game, and a line "Headset N fps Game N fps (paused)". | capture; logcat `XR menu: open (game paused)` |
| F2 | Stick up/down; hold down 2 s. | The highlighted line moves one step, then repeats; wraps around. | capture |
| F3 | Left ≡, B, or Resume + A. | The menu closes, the game continues where it was; the A/B press doesn't reach the game. | logcat `XR menu: closed`; tester's note |
| F4 | Recenter view + A, facing 90° away. | Menu closes; the view is recentred to where you face. | logcat `XR: game camera placed at the head` |
| F5 | World size left/right. | Each step 10% smaller/bigger, at once; kept after a restart (emu.cfg `vr.WorldScale`). | capture before/after; emu.cfg |
| F6 | Start: right stick click only. Left ≡ no longer pauses the game through its own menu. | As described. | tester's note |
| E1 | Game behind the menu = Running; then change MSAA Off → 2x → 4x. | Each change rebuilds the eye buffers within a frame (a short blink at most); edges visibly smoother at 4x. Note the headset and game fps on the menu line for each value. | logcat `XR: eye buffer settings changed (MSAA 4 ...)` then `XR: eye buffers WxH, 4x MSAA`; fps per value |
| E2 | Anisotropic filtering Off → 16x. | Ground textures at a grazing angle sharper; fps noted. | capture of the same view per value; fps |
| E3 | Sharper far textures Off → Low → High. | Distant textures sharper; High may shimmer while moving (note it). | captures; fps |
| E4 | Resolution multiplier 100% → 160%, one step at a time (Game behind the menu = Running). | Sharper picture as it grows; eye buffers capped at the runtime's maximum (logcat size stops growing); fps drops at some point: note where it falls below 72. | logcat `XR: eye buffers WxH` per step; fps per value |
| E5 | Restart the app. | The chosen values are kept (emu.cfg `vr.Msaa`, `rend.AnisotropicFiltering`, `vr.TexLodBias`, `vr.XrResolution`, `vr.ComfortVignette`). | emu.cfg |
| E6 | Pick the best quality that holds 72 fps in the busiest level tried. | A recommended default set for the release. | table in the results |
| F7 | Export saves + A. | Status line "Saves exported to Download/ToyCommanderVR/Saves/<time>"; the folder has the VMU files. | `adb shell ls` of the folder |
| F8 | Import saves + A, then A again. | "Saves imported. Restarting..."; the app restarts by itself within ~2 s and the game boots with those saves. | logcat; tester's note |
| F9 | Quit game + A, A. | Back to Horizon Home; the app's process is gone. | `adb shell pidof com.dreamcast4ever.toycommandervr` empty |
| F10 | Vignette off while the menu is open (stick moves with menu open). | No vignette. | capture |

## H. HD texture upscale cache

| # | Steps | Expected | Evidence |
|---|---|---|---|
| H1 | Fresh install (or delete `files/.../texcache/`), default settings (2x). Play the first mission 3 minutes. | Game runs as before (no new stutter); logcat `tcvr: upscale cache: 1 textures upscaled 2x`, then every 50; the menu's status line "HD textures N" grows. | logcat; menu capture |
| H2 | `"$ADB" shell run-as com.dreamcast4ever.toycommandervr find . -path '*texcache*' -name '*.png' \| wc -l` and `du -sh` on the folder. | PNG files appear; size noted (MB per level). | command output |
| H3 | Quit and relaunch, same mission. | logcat `tcvr: upscale cache: N textures in ...` at start; textures visibly sharper (no blocky pixels) from the first frame; fewer new textures upscaled. | captures before/after at the same spot |
| H4 | Menu: HD textures Off / 2x / 4x. | Off: originals next time the textures load (next level); 4x: new textures cached at 4x (already cached ones stay 2x). | captures; logcat |
| H5 | Look for artefacts: text, HUD digits, transparent edges, the radar. | Note any texture that looks worse upscaled (its hash from the cache file names, if needed). | captures |
| H6 | Performance: headset/game fps with cache on vs off in the same scene. | No loss once the cache is built. | fps |

## G. Launch splash and loading panel

| # | Steps | Expected | Evidence |
|---|---|---|---|
| G1 | Start the app from the library (worn). | The system splash "TOY COMMANDER VR / Loading..." instead of a black void, then the loading panel "Reading your disc..." / "Starting Toy Commander..." with "Disc MK-5102050", then the game. | tester's note; logcat `XR: loading panel shown` |
| G2 | Time from the tap to the first game picture. | Noted (seconds). | stopwatch |
| G3 | Only a wrong disc (A3). | Loading panel, then back to the setup panel with the red message. | as A3 |

## Regression checks (every run)

- Start mapping unchanged: right stick click and left ≡ are Start; left stick click recenters.
- Audio: sound present and normal speed (the audio PR's checks).
- Menu and view sizing unchanged (no change made there; compare with an earlier capture).
- emu.cfg and VMU hashes unchanged by the tests (only the app's own `vr-refused-discs.properties` is new
  in `files/` after A3).

## Reporting

For each row: pass / fail / not run, the evidence file names, the APK version and SHA-256. Failed rows:
logcat excerpt around the failure. Results go in the PR descriptions (one PR per area) and in
`AI/Prompts_Records/<timestamp>_P0_HeadsetTest.md` in the private hub.
