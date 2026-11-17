# Installing Toy Commander VR on a Meta Quest

Toy Commander VR is an unofficial fan project: a Dreamcast emulator made for the Quest that plays
**your own copy** of Toy Commander in VR. The app contains nothing of the game. You need:

- a Meta Quest 3 (Quest 3S and Quest 2 may work but aren't tested yet);
- a PC (Windows, macOS or Linux) and a USB-C cable that carries data;
- the app's APK file (`ToyCommanderVR-<version>.apk`), from the latest release on the repository's
  **Releases** page;
- your own disc image of **Toy Commander (Europe) (En,Fr,De,Es)**, game ID `MK-5102050`, as a
  `.chd`, a `.gdi` with its track files, a `.cue` with its `.bin` files, or a `.cdi`. Other versions
  (US, Japan) are refused: every VR adjustment is made for the European disc.

## 1. Turn on developer mode (once)

Apps from outside the Meta store need developer mode.

1. Create or join a developer organisation at https://developers.meta.com (free; a name is enough).
2. In the Meta Horizon app on your phone: **Devices** → your headset → **Headset settings** →
   **Developer mode** → on.
3. Restart the headset.

## 2. Install the app

Pick one of these.

**SideQuest (easiest).** Install SideQuest (the "Advanced installer") on the PC, connect the
headset with the cable, put the headset on and accept **Allow USB debugging** (tick "Always allow
from this computer"). In SideQuest, use **Install APK file from folder** and pick the APK.

**Meta Quest Developer Hub.** Connect the headset, then drag the APK onto the device panel.

**adb (command line).** With the Android platform tools installed:

```bash
adb install -r ToyCommanderVR-<version>.apk
```

To update later, install the new APK the same way: your saves and settings stay.

Genuine releases are signed with the project's release key. Its certificate SHA-256 fingerprint is
`F9:FD:0F:5A:E6:A4:E8:7C:8B:F8:3F:3B:FA:0C:5B:02:AB:0D:74:73:88:9C:B7:28:D5:4B:93:16:BD:7B:B8:58`
(check it with `apksigner verify --print-certs ToyCommanderVR-<version>.apk`, from the Android SDK
build tools).

## 3. Copy your disc image

Copy the disc image to the headset's **Download** folder, ideally in `Download/ToyCommanderVR/`:

- **SideQuest:** **Manage files on the headset** → `Download` → create `ToyCommanderVR` → upload.
- **Windows Explorer / macOS Android File Transfer:** with the cable connected, put the headset on
  and allow file access, then open **Quest → Internal shared storage → Download**.
- **adb:** `adb push "Toy Commander (Europe).chd" /sdcard/Download/ToyCommanderVR/`

A `.gdi` or `.cue` needs all the track files it lists next to it. Unpack `.zip` or `.7z` archives
on the PC first.

## 4. First launch: let the app read your files

1. On the headset: **Library** → filter **Unknown sources** → **Toy Commander VR**.
2. The first time, a setup panel opens. Under **1. Let the app read your files**, tap **Allow**:
   Android's **All files access** page opens. Switch **Allow access to manage all files** on, then
   come back (the panel shows **Done.**).
3. Under **2.** the panel shows the disc image it found. Tap **Play Toy Commander**.
   - "isn't the European Toy Commander disc": that file is another version; use the European one.
   - "not the files it lists next to it": copy the missing track files to the same folder.
4. The game starts in VR after a short loading panel. From then on, the app starts the game
   directly.

## 5. Playing

Controls: see `tcvr/docs/Controls.md`. In short: left stick drives, the left ≡ button is Start,
hold it (or press it twice) for the **VR menu** (recenter, comfort, picture settings, saves, quit),
right stick click recenters.

The first time you play each level, the app makes HD versions of its textures in the background;
they are reused from then on (VR menu → HD textures to turn this off).

## Saves

Your saves are kept inside the app (four memory cards' worth) and backed up automatically at each
launch (the five newest backups are kept).

- **Export:** VR menu → **Export saves to Download** → copies to `Download/ToyCommanderVR/Saves/<time>/`.
- **Import:** put a saves folder in `Download/ToyCommanderVR/Saves/`, then VR menu → **Import
  saves from Download** (press A twice); the game restarts with them.

Uninstalling the app deletes its saves: export them first.

## Troubleshooting

| Problem | What to do |
|---|---|
| "App not installed" or a signature conflict when updating | An earlier test build is installed (signed with a different key). VR menu → Export saves, uninstall the app, install the new APK, then import the saves |
| The app isn't in the library | Filter on **Unknown sources**; check that developer mode is on |
| The setup panel keeps asking for access | Settings → Apps → Toy Commander VR → Permissions → Files: allow all |
| "isn't the European Toy Commander disc" | Use the European disc image (`MK-5102050`) |
| Black view or stuck on the loading panel | Quit (right ≡ → Quit), start again; restart the headset if it persists |
| No sound | Restart the headset (a known Horizon OS audio state) |
| The view is off to one side | Right stick click (recenter) |
| Motion sickness | VR menu → Comfort vignette → High; take breaks |

## Legal

Unofficial, non-commercial fan project, not affiliated with or endorsed by Sega or No Cliché.
Toy Commander is a trademark of its owners. The app is built on Flycast and is free software under
the GNU GPL (version 2 or later); its source code is published with each release. It contains no
game data: play only a disc image made from a copy you own.
