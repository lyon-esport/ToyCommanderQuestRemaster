# Toy Commander VR: controls (Meta Quest Touch controllers)

The Touch controllers act as a Dreamcast pad. Your head only turns your view: the vehicle steers
and aims with the sticks and buttons, as in the original game.

## In the game

| Touch controller | Dreamcast pad | What it does in Toy Commander |
|---|---|---|
| Left stick | Analog stick | Steer and drive or fly the vehicle |
| Right stick | D-pad (8 directions) | Menus; the D-pad's in-game uses |
| Left trigger | L trigger | as on the Dreamcast pad |
| Right trigger | R trigger | as on the Dreamcast pad |
| A, B, X, Y | A, B, X, Y | as on the Dreamcast pad (A: confirm in menus, B: back) |
| **Left ≡ button, short press** | **Start** | The game's own pause menu |
| **Left ≡ button, held 0.6 s or pressed twice** | (none) | Opens the **VR menu** (see below) |
| **Right stick click** | (none) | **Recenter**: the view faces where you look now |
| Grip + right stick up/down | (none) | Make the world bigger or smaller (kept for next time) |

Notes:
- A single press of the left ≡ is sent to the game 0.25 s after you let go (the time to tell it
  from a double press).
- The right ≡ (Meta) button belongs to the headset: it opens Horizon OS's own menu.
- The left stick click does nothing for now.

## The VR menu

Hold the left ≡ button (0.6 s) or press it twice. The game **freezes** behind the menu and stays
frozen until you close it, so nothing happens to your vehicle meanwhile. The menu appears in
front of where you look.

| Control | In the VR menu |
|---|---|
| Either stick up/down | Move between lines (hold to repeat) |
| Either stick left/right | Change the value on the line |
| A | Choose (Resume, Recenter, Export...) or next value |
| B, or a press of the left ≡ | Close the menu, back to the game |

Lines:

| Line | What it does |
|---|---|
| Resume | Back to the game |
| Recenter view | Recenter, then back to the game |
| World size | Smaller/bigger world, 10% per step |
| Comfort vignette | Darkens the edges of the view while the vehicle turns: Off, Low, Medium, High |
| Anti-aliasing (MSAA) | Smoother edges: Off, 2x, 4x |
| Anisotropic filtering | Sharper ground textures at an angle: Off to 16x |
| Sharper far textures | Sharper textures in the distance: Off, Low, High |
| HD textures (upscale cache) | Upscaled textures made once from your disc as you play: Off, 2x, 4x |
| Resolution multiplier | More pixels per eye, sharper picture: 100% to 160% |
| Export saves to Download | Copies your saves to Download/ToyCommanderVR/Saves/ |
| Import saves from Download | Restores the newest exported saves (press A twice); the game restarts |
| Quit game | Back to the headset's home (press A twice) |

The last line shows the headset's frame rate and the number of HD textures made so far. Picture
settings take effect at once and are kept for next time; the higher ones cost frame rate (see
`tcvr/docs/PerformanceTargets.md`).
