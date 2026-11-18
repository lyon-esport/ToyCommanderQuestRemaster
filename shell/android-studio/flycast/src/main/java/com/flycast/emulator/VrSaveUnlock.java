// tcvr: "Unlock all levels" for Toy Commander (PAL, MK-5102050): opens every mission in the saved
// games of a VMU image. Pure Java (no Android), so it can be tested on a desktop JVM
// (tcvr/tools/tests/VrSaveUnlockTest.java).
//
// Format (our own reverse engineering, tcvr/docs/Saves.md and tcvr/docs/RE/Addresses.md):
// a VMU image is 256 blocks of 512 bytes; root block 255, FAT and directory blocks named there.
// Each player's progress is a data file "TOY_COMM.<3-letter name>" (the system file is
// "TOY_COMM.___"): a VMS header with one icon and no eye-catch (640 bytes), then 384 bytes:
//   0xCC  u16[7] rooms 1..7: 2 bits per mission 1..7 (0 locked, 1 open, 2 done, 3 done under par)
//   0xDA  u16    extra missions: bits 0-1 extra 1, bits 2-3 extra 2 (the final)
//   0xDC  u16    rooms unlocked mask (bit r-1 = room r; bit 7 = the extras)
//   0xE3  u8     1 = a valid saved game
// The game keeps no checksum (it leaves the VMS CRC at 0), so only these bytes change.
// Unlocking raises every locked mission to 1 (open) and sets the 8 room bits, like the game's
// own "all missions" cheat: completed missions, best times and records stay as they are.
package com.flycast.emulator;

import java.nio.charset.StandardCharsets;

public final class VrSaveUnlock {
    public static final String GAME_ID = "MK-5102050";
    public static final int CARD_SIZE = 256 * 512;

    static final int PAYLOAD_SIZE = 384;
    static final int HEADER_SIZE = 640;		// 128-byte VMS header + one 512-byte icon
    static final int STATUS = 0xCC, EXTRAS = 0xDA, ROOMS = 0xDC, VALID = 0xE3;

    private VrSaveUnlock() {}

    /** What patch() did to one card. */
    public static final class Result {
        public int savedGames;		// saved games found and accepted
        public int changed;			// of them, changed (others were already unlocked)
        public int rejected;		// TOY_COMM files that did not look as expected (left alone)
        public final StringBuilder log = new StringBuilder();
    }

    private static int u16(byte[] b, int o) { return (b[o] & 0xff) | (b[o + 1] & 0xff) << 8; }
    private static long u32(byte[] b, int o) { return u16(b, o) | (long) u16(b, o + 2) << 16; }
    private static void put16(byte[] b, int o, int v) { b[o] = (byte) v; b[o + 1] = (byte) (v >> 8); }

    private static String ascii(byte[] b, int o, int n) {
        return new String(b, o, n, StandardCharsets.US_ASCII);
    }

    /** True for a file name of the Toy Commander per-game VMU image. */
    public static boolean isToyCommanderCard(String fileName) {
        return fileName.startsWith(GAME_ID + "_vmu_save_") && fileName.endsWith(".bin");
    }

    /**
     * The player names of the saved games on the card (the file names are "TOY_COMM.<name>"; the
     * options file "TOY_COMM.___" is not a saved game). Empty for a card that doesn't look right. Never throws.
     */
    public static java.util.List<String> playerNames(byte[] card) {
        java.util.List<String> names = new java.util.ArrayList<>();
        if (card == null || card.length != CARD_SIZE)
            return names;
        int root = 255 * 512;
        for (int i = 0; i < 16; i++)
            if ((card[root + i] & 0xff) != 0x55)
                return names;
        int dirBlock = u16(card, root + 0x4a), dirSize = u16(card, root + 0x4c);
        if (dirBlock > 255 || dirSize == 0 || dirSize > dirBlock + 1)
            return names;
        for (int d = 0; d < dirSize; d++) {
            int blk = (dirBlock - d) * 512;
            for (int e = 0; e < 16; e++) {
                int ent = blk + e * 32;
                if (card[ent] == 0)
                    continue;
                String name = ascii(card, ent + 4, 12);
                if (!name.startsWith("TOY_COMM.") || name.equals("TOY_COMM.___"))
                    continue;
                String player = name.substring(9).trim();
                if (!player.isEmpty() && player.indexOf('\0') < 0)
                    names.add(player);
            }
        }
        return names;
    }

    /** The new value of a 2-bits-per-mission word: every 0 field of the first n becomes 1. */
    static int openAll(int word, int n) {
        for (int m = 0; m < n; m++)
            if (((word >> (2 * m)) & 3) == 0)
                word |= 1 << (2 * m);
        return word;
    }

    /**
     * Unlocks every saved game of the card in place (card = the whole 128 KB image). Changes
     * nothing when the card or a file doesn't look as expected. Never throws on bad data.
     */
    public static Result patch(byte[] card) {
        Result r = new Result();
        if (card == null || card.length != CARD_SIZE) {
            r.log.append("not a VMU image; ");
            return r;
        }
        int root = 255 * 512;
        for (int i = 0; i < 16; i++)
            if ((card[root + i] & 0xff) != 0x55) {
                r.log.append("unformatted card; ");
                return r;
            }
        int fatBlock = u16(card, root + 0x46), dirBlock = u16(card, root + 0x4a), dirSize = u16(card, root + 0x4c);
        if (fatBlock > 255 || dirBlock > 255 || dirSize == 0 || dirSize > dirBlock + 1) {
            r.log.append("bad root block; ");
            return r;
        }
        int fat = fatBlock * 512;
        for (int d = 0; d < dirSize; d++) {
            int blk = (dirBlock - d) * 512;
            for (int e = 0; e < 16; e++) {
                int ent = blk + e * 32;
                if (card[ent] == 0)
                    continue;
                String name = ascii(card, ent + 4, 12);
                if (!name.startsWith("TOY_COMM.") || name.equals("TOY_COMM.___"))
                    continue;
                String why = patchFile(card, ent, fat, r);
                if (why != null) {
                    r.rejected++;
                    r.log.append(name).append(": ").append(why).append("; ");
                }
            }
        }
        return r;
    }

    // Returns null when the file was accepted (and patched if needed), else why it was left alone.
    private static String patchFile(byte[] card, int ent, int fat, Result r) {
        if ((card[ent] & 0xff) != 0x33)
            return "not a data file";
        int first = u16(card, ent + 2), blocks = u16(card, ent + 0x18), headerBlock = u16(card, ent + 0x1a);
        if (blocks != 2 || headerBlock != 0 || first > 255)
            return "unexpected size";
        int second = u16(card, fat + first * 2);
        if (second > 255 || u16(card, fat + second * 2) != 0xfffa)
            return "unexpected block chain";
        int head = first * 512;
        if (!ascii(card, head, 14).equals("TOY SAVED GAME")
                || !ascii(card, head + 16, 26).equals("TOY COMMANDER : SAVED GAME"))
            return "not a Toy Commander saved game";
        if (u16(card, head + 0x40) != 1 || u16(card, head + 0x44) != 0 || u32(card, head + 0x48) != PAYLOAD_SIZE)
            return "unexpected header";
        // the payload starts at file offset 640: offset 128 of the second block, all of it in that block
        int p = second * 512 + (HEADER_SIZE - 512);
        if (card[p + VALID] != 1)
            return "not marked valid";
        int rooms = u16(card, p + ROOMS);
        if ((rooms & ~0xff) != 0 || (rooms & 1) == 0)
            return "unexpected rooms mask";
        for (int i = 0; i < 7; i++)
            if (u16(card, p + STATUS + 2 * i) > 0x3fff)
                return "unexpected mission status";
        if (u16(card, p + EXTRAS) > 0xf)
            return "unexpected extra status";

        r.savedGames++;
        boolean changed = false;
        for (int i = 0; i < 7; i++) {
            int o = p + STATUS + 2 * i, v = u16(card, o), n = openAll(v, 7);
            if (n != v) { put16(card, o, n); changed = true; }
        }
        int ex = u16(card, p + EXTRAS), exn = openAll(ex, 2);
        if (exn != ex) { put16(card, p + EXTRAS, exn); changed = true; }
        if (rooms != 0xff) { put16(card, p + ROOMS, 0xff); changed = true; }
        if (changed)
            r.changed++;
        return null;
    }
}
