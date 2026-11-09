# Saved games (VMU memory cards)

Toy Commander VR stores its saves like Flycast: as virtual memory card (VMU) files.

## Where they live
`/sdcard/Android/data/com.dreamcast4ever.toycommandervr/files/data/`
(older test builds: `com.flycast.emulator.vr`)

- `MK-5102050_vmu_save_A1.bin` - per-game card in port A1 (the 4 game slots live inside this card)
- `vmu_save_A2.bin` - second card, if used

## Automatic backups
At every launch the app copies all `*vmu_save_*.bin` files to
`.../files/save_backups/<YYYYMMDD_HHMMSS>/`. The 5 most recent backups are kept.

## Export / import (in-VR menu)
- Export: copies the saves to `/sdcard/Download/ToyCommanderVR/Saves/<YYYYMMDD_HHMMSS>/`.
- Import: restores the newest folder from that location. Your current saves are backed up first.
  Only possible while no game is running; the game restarts afterwards.

Both need "All files access" for the app.

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
