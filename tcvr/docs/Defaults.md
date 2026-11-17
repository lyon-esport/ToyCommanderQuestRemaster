# Shipped VR defaults

The Quest build starts with these settings when `emu.cfg` has no value for them
(first launch, or a deleted config). They were tuned on a Quest 3 on 2026-11-17 and are set in
`core/cfg/option.cpp`. Players change them in the VR menu (left ≡); the menu saves to `emu.cfg`.

| VR menu line | Option | Default | Stock Flycast/hotd2-vr |
|---|---|---|---|
| Resolution multiplier | `vr.XrResolution` | 100% (1.0) | 1.0 |
| Anti-aliasing (MSAA) | `vr.Msaa` | 2x | Off |
| Anisotropic filtering | `rend.AnisotropicFiltering` | 4x (OpenXR builds) | Off |
| Sharper far textures | `vr.TexLodBias` | Off (0) | 0 |
| Textures | `vr.UpscaleCache` | HD 2x | HD 2x |
| Refresh rate | `vr.XrRefreshRate` | 120 Hz | 72 Hz |
| Show frame rate | `vr.ShowFps` | On | Off |
| Fixed foveation (edges) | `vr.Foveation` | Off | Off |
| Upload frame once | `vr.UploadOnce` | Off | On |
| Discard eye depth | `vr.DiscardDepth` | On | On |
| Comfort vignette | `vr.ComfortVignette` | Low (1; the former High) | 0.6 (the former Medium) |
| World size | `vr.WorldScale` | 182% (0.2, the menu's maximum) | 100% (0.11) |

The headset can refuse a refresh rate (a developer lock, or another app such as Quest Games
Optimizer). The game then keeps the rate the system gives it; the VR menu still shows the request.
