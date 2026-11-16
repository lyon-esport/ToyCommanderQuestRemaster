# Toy Commander Quest Remaster

Fan VR adaptation of the Dreamcast game *Toy Commander* for standalone Meta Quest headsets.
It is a fork of the [Flycast](https://github.com/flyinghead/flycast) Dreamcast emulator, by
way of [The House of the Dead 2 VR](README.hotd2-vr.md). There is no remake here: the
original game runs, and the emulator turns what it draws into a VR view, with per-game
memory patches (no game file is modified or distributed).

> **No game data is included.** You must own the game and supply your own disc image.
> Only the European (PAL) release, product ID `MK-5102050`, is supported.

Status: early development (private). What's next: see the [roadmap](ROADMAP.md).

## Planned install (for players)
1. Sideload the APK (SideQuest or `adb install`).
2. Copy your own Toy Commander disc image (`.chd` or `.gdi`) to `Download/ToyCommanderVR/` on the headset.
3. Launch *Toy Commander VR* and grant "All files access" when the setup panel asks.

## Controls

Meta Touch controllers act as a Dreamcast pad:

| Touch controller | Dreamcast / VR |
|---|---|
| Left thumbstick | Analog stick |
| Right thumbstick | D-pad (8 directions) |
| Left trigger / right trigger | L / R triggers |
| A, B (right) / X, Y (left) | A, B / X, Y |
| Left ≡ (menu), short press | Start (the game's menu) |
| Left ≡ (menu), long press or double press | VR menu (planned) |
| Right thumbstick click | Recenter the view |
| Hold a grip, then right thumbstick up / down | Make the world bigger / smaller (saved) |

The view also recenters by itself when you come back to the headset after more than a few
seconds away (system menu, headset taken off).

The left thumbstick click does nothing. The VR menu (planned, see the roadmap) is drawn over
the paused game: recenter, world size, saves, quit.

## Saves

Saves are Dreamcast memory card (VMU) files, backed up automatically at every launch, with
export / import to `Download/ToyCommanderVR/Saves/` and a restore path that needs no menu.
Details: [tcvr/docs/Saves.md](tcvr/docs/Saves.md).

## Credits

- [Flycast](https://github.com/flyinghead/flycast) by flyinghead and contributors. Flycast's
  own readme: [README.flycast.md](README.flycast.md).
- [The House of the Dead 2 VR](https://github.com/mikermak/hotd2-vr) by mikermak, the VR
  base of this project (OpenXR host, stereo reprojection, netplay fixes). Its readme and
  third-party credits: [README.hotd2-vr.md](README.hotd2-vr.md#credits-and-licences).
- [OpenXR loader](https://github.com/KhronosGroup/OpenXR-SDK) by Khronos, Apache-2.0.
- Roboto (on the headset's panels) by Google, Apache-2.0.
- "Namco Arcade Gun" model by [Martoscar](https://sketchfab.com/Martoscar), CC BY 4.0: in
  the source tree, inherited from HOTD2 VR, but not in Toy Commander VR builds (see below).
- Toy Commander VR: Lyon e-Sport.

## License and source

This project's code is licensed under the GNU General Public License, version 2 or (at your
option) any later version, like Flycast; see [LICENSE](LICENSE). Changed Flycast files carry
a note.

The source tree keeps HOTD2 VR's arcade gun model (`core/rend/vr/gun_model.h`) under its own
licence, CC BY 4.0. Toy Commander VR builds **leave it out** (CMake option `TCVR_GUN_MODEL`,
off by default; Toy Commander is played with a pad and never shows the gun), so **the APKs are
distributed under GPL-2.0-or-later**, like Flycast. CC BY 4.0 is compatible with GPL-3.0 but not
GPL-2.0: a build made with `-DTCVR_GUN_MODEL=ON` includes the model and is then distributed under
GPL-3.0, which the "or later" clause allows.

**Source offer:** whoever receives an APK or other binary of this project is entitled to its
complete corresponding source code. Each released build is published together with the
exact source it was built from (the matching tag in this repository, or a source archive
attached to the release).

*Toy Commander* is © its respective owners (originally developed by No Cliché and published
by Sega). Dreamcast and Sega are trademarks of Sega. Meta Quest is a trademark of Meta.
This project is not affiliated with or endorsed by any of them, and contains no Sega code,
data, graphics or sound. The public name and icon of the app are still to be chosen (see
roadmap item P0-7).
