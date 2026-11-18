// Desktop test of VrSaveUnlock (the "Unlock all levels" patcher). No headset, no game data needed:
// it builds a synthetic VMU image. Optionally checks real cards given on the command line
// (read-only: they are patched in memory and the result is printed, never written).
//
//   bash tcvr/tools/tests/unlock-test.sh [card.bin ...]
import com.flycast.emulator.VrSaveUnlock;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.Arrays;

public class VrSaveUnlockTest {
    static int fails = 0;

    static void check(boolean ok, String what) {
        System.out.println((ok ? "ok   " : "FAIL ") + what);
        if (!ok) fails++;
    }

    static int u16(byte[] b, int o) { return (b[o] & 0xff) | (b[o + 1] & 0xff) << 8; }
    static void put16(byte[] b, int o, int v) { b[o] = (byte) v; b[o + 1] = (byte) (v >> 8); }
    static void put(byte[] b, int o, String s) { byte[] a = s.getBytes(StandardCharsets.US_ASCII); System.arraycopy(a, 0, b, o, a.length); }

    // A formatted card with the given TOY_COMM files: name -> {first block, second block}
    static byte[] card() {
        byte[] c = new byte[256 * 512];
        int root = 255 * 512;
        Arrays.fill(c, root, root + 16, (byte) 0x55);
        put16(c, root + 0x46, 254); put16(c, root + 0x48, 1);
        put16(c, root + 0x4a, 253); put16(c, root + 0x4c, 13);
        for (int i = 0; i < 256; i++) put16(c, 254 * 512 + i * 2, 0xfffc);
        return c;
    }

    static int addSavedGame(byte[] c, int slot, String name, int first, int second, String vm) {
        int ent = 253 * 512 + slot * 32;
        c[ent] = 0x33;
        put16(c, ent + 2, first);
        put(c, ent + 4, "TOY_COMM." + name);
        put16(c, ent + 0x18, 2);
        put16(c, ent + 0x1a, 0);
        put16(c, 254 * 512 + first * 2, second);
        put16(c, 254 * 512 + second * 2, 0xfffa);
        int h = first * 512;
        put(c, h, vm);
        put(c, h + 16, "TOY COMMANDER : SAVED GAME      ");
        put16(c, h + 0x40, 1);
        put16(c, h + 0x48, 384);
        int p = second * 512 + 128;
        put16(c, p + 0xcc, 0x05a6);		// room 1: done, open, done, done, open, open, locked
        put16(c, p + 0xce, 0x0005);		// room 2: m1, m2 open
        put16(c, p + 0xdc, 0x0003);
        c[p + 0xe3] = 1;
        c[p] = 42;						// a best time, must stay
        return p;
    }

    public static void main(String[] args) throws Exception {
        byte[] c = card();
        int p = addSavedGame(c, 0, "ABC", 199, 198, "TOY SAVED GAME  ");
        int q = addSavedGame(c, 1, "XYZ", 197, 196, "TOY SAVED GAME  ");
        c[q + 0xe3] = 0;							// invalid: must be left alone
        byte[] before = c.clone();
        VrSaveUnlock.Result r = VrSaveUnlock.patch(c);
        check(r.savedGames == 1 && r.changed == 1 && r.rejected == 1, "one saved game patched, one rejected: " + r.log);
        check(u16(c, p + 0xcc) == 0x15a6, "room 1: done/open kept, mission 7 opened (0x" + Integer.toHexString(u16(c, p + 0xcc)) + ")");
        check(u16(c, p + 0xce) == 0x1555, "room 2: all open");
        boolean rooms = true;
        for (int i = 2; i < 7; i++) rooms &= u16(c, p + 0xcc + 2 * i) == 0x1555;
        check(rooms, "rooms 3-7: all open");
        check(u16(c, p + 0xda) == 0x0005, "both extra missions open");
        check(u16(c, p + 0xdc) == 0x00ff, "rooms mask 0xFF");
        check(c[p] == 42, "best time kept");
        int diff = 0;
        for (int i = 0; i < c.length; i++) if (c[i] != before[i]) diff++;
        check(diff <= 18, "only status bytes changed (" + diff + ")");
        check(Arrays.equals(Arrays.copyOfRange(c, q - 128, q + 384), Arrays.copyOfRange(before, q - 128, q + 384)), "rejected file untouched");

        // the multiplayer's default player name: the saved games' names, in directory order (the options file isn't one)
        byte[] named = card();
        addSavedGame(named, 0, "ABC", 199, 198, "TOY SAVED GAME  ");
        addSavedGame(named, 1, "XYZ", 197, 196, "TOY SAVED GAME  ");
        int opt = 253 * 512 + 2 * 32;
        named[opt] = 0x33;
        put(named, opt + 4, "TOY_COMM.___");
        check(VrSaveUnlock.playerNames(named).equals(Arrays.asList("ABC", "XYZ")), "player names: " + VrSaveUnlock.playerNames(named));
        check(VrSaveUnlock.playerNames(new byte[256 * 512]).isEmpty() && VrSaveUnlock.playerNames(null).isEmpty(),
                "no player names on an unformatted card");

        byte[] again = c.clone();
        VrSaveUnlock.Result r2 = VrSaveUnlock.patch(again);
        check(r2.changed == 0 && Arrays.equals(again, c), "second run changes nothing");

        byte[] blank = new byte[256 * 512];
        check(VrSaveUnlock.patch(blank).savedGames == 0 && Arrays.equals(blank, new byte[256 * 512]), "unformatted card untouched");
        byte[] other = card();
        addSavedGame(other, 0, "ABC", 199, 198, "OTHER GAME SAVE ");
        byte[] otherBefore = other.clone();
        check(VrSaveUnlock.patch(other).savedGames == 0 && Arrays.equals(other, otherBefore), "other game's file untouched");
        check(VrSaveUnlock.isToyCommanderCard("MK-5102050_vmu_save_A1.bin") && !VrSaveUnlock.isToyCommanderCard("vmu_save_A1.bin"),
                "only the Toy Commander card file name");

        for (String path : args) {
            byte[] real = Files.readAllBytes(Paths.get(path));
            byte[] copy = real.clone();
            VrSaveUnlock.Result rr = VrSaveUnlock.patch(copy);
            int d = 0;
            for (int i = 0; i < copy.length; i++) if (copy[i] != real[i]) d++;
            System.out.println("card " + Paths.get(path).getFileName() + ": " + rr.savedGames + " saved game(s), " + rr.changed
                    + " changed, " + rr.rejected + " rejected, " + d + " byte(s) differ " + rr.log);
        }
        System.out.println(fails == 0 ? "ALL PASSED" : fails + " FAILED");
        System.exit(fails == 0 ? 0 : 1);
    }
}
