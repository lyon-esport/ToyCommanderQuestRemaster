# Saved games (VMU memory cards)

Toy Commander VR stores its saves like Flycast: as virtual memory card (VMU) files.

## Where they live
`/sdcard/Android/data/com.dreamcast4ever.toycommandervr/files/data/`
(older test builds: `com.flycast.emulator.vr`)

- `MK-5102050_vmu_save_A1.bin` - per-game card in port A1. Toy Commander keeps one system file
  (`TOY_COMM.___`: options, records) and one saved game per player name (`TOY_COMM.<name>`) on it
- `vmu_save_A2.bin` - second card, if used

## Automatic backups
At every launch the app copies all `*vmu_save_*.bin` files to
`.../files/save_backups/<YYYYMMDD_HHMMSS>/`. The 5 most recent backups are kept.

## Export / import (in-VR menu)
- Export: copies the saves to `/sdcard/Download/ToyCommanderVR/Saves/<YYYYMMDD_HHMMSS>/`.
- Import: restores the newest folder from that location. Your current saves are backed up first.
  Only possible while no game is running; the game restarts afterwards.

- Unlock all levels (Toy Commander only): opens every mission of every room, the bonus missions and
  the two extra missions in all the saved games on the card (A twice to confirm). Your saves are backed
  up first (`save_backups`, and exported to Download when allowed), then the game restarts. Missions
  you completed, best times and records stay as they are: locked missions become "open", like the
  game's own cheat. A player name must exist (start a game once); nothing is written otherwise.

Export and import need "All files access" for the app.

### How "Unlock all levels" works
`VrSaveUnlock.java` patches the saved-game files inside `MK-5102050_vmu_save_*.bin`, in place (only
the changed bytes; the file keeps its owner). For each `TOY_COMM.<name>` data file it checks the card
format, the directory entry and FAT chain (2 blocks), the VMS header (`TOY SAVED GAME`, one icon, no
eye-catch, 384 bytes of data) and the data itself (valid flag, rooms mask and status ranges); any
mismatch leaves that file untouched. In the 384 bytes (after the 640-byte header):

| Offset | Field | Unlock |
|---|---|---|
| `0xCC` | u16 x 7, rooms 1-7: 2 bits per mission 1-7 (0 locked, 1 open, 2 completed, 3 completed under the target time) | every 0 becomes 1 |
| `0xDA` | u16, extra missions (bits 0-1, 2-3) | every 0 becomes 1 |
| `0xDC` | u16, rooms unlocked (bit r-1 = room r, bit 7 = extras) | `0xFF` |

The game keeps no checksum (the VMS CRC stays 0). In RAM these are `0x8C0C7DC4` and `0x8C0A5A5C`
(see `RE/Addresses.md`). The running game holds its progress in RAM, which is why the game restarts.
Desktop test: `bash tcvr/tools/tests/unlock-test.sh [card.bin ...]` (synthetic card; real cards are
only read).

## Manual restore (SideQuest / adb)
1. Close the game.
2. `adb push MK-5102050_vmu_save_A1.bin /sdcard/Android/data/com.dreamcast4ever.toycommandervr/files/data/`
   (or copy the file with SideQuest's file manager, or put a folder in Download/ToyCommanderVR/Saves and use Import).
3. Relaunch. To fetch backups: `adb pull /sdcard/Android/data/com.dreamcast4ever.toycommandervr/files/save_backups`.

## Restore without the VR menu (SideQuest / file manager)
1. Copy a save folder (with its `*vmu_save_*.bin` files) into `Download/ToyCommanderVR/Saves/<any name>/`.
2. Create an empty file `Download/ToyCommanderVR/Saves/import.request`.
3. Start Toy Commander VR: the newest folder in `Saves/` is imported (your current saves are backed up first),
   and `import.request` is deleted.
