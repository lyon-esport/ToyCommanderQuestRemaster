# Shipped VR defaults

The Quest build starts with these settings when `emu.cfg` has no value for them
(first launch, or a deleted config). They were tuned on a Quest 3 on 2026-11-17 and are set in
`core/cfg/option.cpp`. Players change them in the VR menu (left ≡); the menu saves to `emu.cfg`.

| VR menu line | Option | Default | Stock Flycast/hotd2-vr |
|---|---|---|---|
| Resolution multiplier | `vr.XrResolution` | 130% (1.3) | 1.0 |
| Anti-aliasing (MSAA) | `vr.Msaa` | 2x | Off |
| Anisotropic filtering | `rend.AnisotropicFiltering` | 4x (OpenXR builds) | Off |
| Sharper far textures | `vr.TexLodBias` | Off (0) | 0 |
| Textures | `vr.UpscaleCache` | HD 2x | HD 2x |
| Refresh rate | `vr.XrRefreshRate` | 120 Hz | 72 Hz |
| Game pictures in missions | `vr.Game60` | 60/s | 30/s |
| Show frame rate | `vr.ShowFps` | On | Off |
| Fixed foveation (edges) | `vr.Foveation` | On (the runtime's "high"; Off/On only) | Off |
| Upload frame once | `vr.UploadOnce` | On | On |
| Discard eye depth | `vr.DiscardDepth` | On | On |
| Comfort vignette | `vr.ComfortVignette` | Low (1; the former High) | 0.6 (the former Medium) |
| World size | `vr.WorldScale` | 182% (0.2, the menu's maximum) | 100% (0.11) |

The headset can refuse a refresh rate (a developer lock, or another app such as Quest Games
Optimizer). The game then keeps the rate the system gives it; the VR menu still shows the request.

## Resolution and frame rate (2026-11-17)

Resolution 130% and the frame counter On were chosen on the headset by feel (worn, Quest 3, stock
eye size). 130% is 1.69x the pixels of 100% and the GPU is the limit (render thread 25–55% of one
core, emulator thread ~23%), so it trades some frame rate for sharpness:

| Resolution, MSAA 2x, 120 Hz | Mean fps | Worst | GPU busy |
|---|---|---|---|
| 100%, Kitchen Training / Parent's bedroom | 119.8–120.4 | 114 | 65–84% |
| 130%, a play session across levels | 116 | 78 | 83% |
| 130%, Kitchen Training start view (heaviest seen) | 87 | 61 | 97–98% |

For a steady 120 at the cost of some sharpness: 115–120% with MSAA 2x, or 130% with MSAA Off.
To keep 130% with MSAA 2x and no drops, set the refresh rate to 90 Hz. In missions the game
draws 30 pictures/s whatever the headset rate; menus draw 60.

