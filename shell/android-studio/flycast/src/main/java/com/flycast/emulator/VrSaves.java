// tcvr: VMU save backup / export / import helper for Toy Commander VR.
//
// Flycast stores VMU memory cards in <home>/data/ where <home> is the "home_directory"
// preference (default: getExternalFilesDir(null), i.e.
// /sdcard/Android/data/<package>/files). Files: vmu_save_A1..D4.bin and
// <gameId>_vmu_save_A1.bin (per-game VMU). All files matching "*vmu_save_*.bin" are handled.
//
// Usage (from the in-VR menu, via JNI or Java):
//   VrSaves.exportToDownload(ctx)         -> "/sdcard/Download/ToyCommanderVR/Saves/<ts>" or "ERROR: ..."
//   VrSaves.importLatestFromDownload(ctx) -> message; ONLY call while the game is NOT running
//                                            (the emulator caches VMU contents in memory and would
//                                            overwrite imported files); restart emulation afterwards.
package com.flycast.emulator;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.Environment;
import android.preference.PreferenceManager;
import android.util.Log;

import com.flycast.emulator.config.Config;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.Locale;

public final class VrSaves {
    private static final String TAG = "tcvr";
    private static final int KEEP_BACKUPS = 5;
    private static final String EXPORT_ROOT = "/sdcard/Download/ToyCommanderVR/Saves";

    private VrSaves() {}

    private static String stamp() {
        return new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(new Date());
    }

    private static File dataDir(Context ctx) {
        String home = null;
        try {
            SharedPreferences p = PreferenceManager.getDefaultSharedPreferences(ctx);
            home = p.getString(Config.pref_home, "");
        } catch (Throwable t) { /* ignore */ }
        File base;
        if (home != null && !home.isEmpty()) {
            base = new File(home);
        } else {
            base = ctx.getExternalFilesDir(null);
            if (base == null) base = ctx.getFilesDir();
        }
        return new File(base, "data");
    }

    private static File backupRoot(Context ctx) {
        File base = ctx.getExternalFilesDir(null);
        if (base == null) base = ctx.getFilesDir();
        return new File(base, "save_backups");
    }

    private static File[] listSaves(File dir) {
        File[] f = dir.listFiles((d, n) -> n.contains("vmu_save_") && n.endsWith(".bin"));
        return f == null ? new File[0] : f;
    }

    private static void copy(File src, File dst) throws Exception {
        try (FileInputStream in = new FileInputStream(src); FileOutputStream out = new FileOutputStream(dst)) {
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            out.getFD().sync();
        }
    }

    /** Copies files into a fresh timestamped folder under root. Returns the folder or null if nothing to copy. */
    private static File copyAll(File[] files, File root) throws Exception {
        if (files.length == 0) return null;
        File dest = new File(root, stamp());
        if (!dest.mkdirs() && !dest.isDirectory()) throw new Exception("cannot create " + dest);
        for (File f : files) copy(f, new File(dest, f.getName()));
        return dest;
    }

    private static File[] sortedDirs(File root) {
        File[] d = root.listFiles(File::isDirectory);
        if (d == null) return new File[0];
        Arrays.sort(d, (a, b) -> a.getName().compareTo(b.getName())); // timestamp names sort chronologically
        return d;
    }

    private static void prune(File root) {
        File[] d = sortedDirs(root);
        for (int i = 0; i < d.length - KEEP_BACKUPS; i++) {
            File[] c = d[i].listFiles();
            if (c != null) for (File f : c) f.delete();
            d[i].delete();
        }
    }

    /** Backs up all VMU saves to files/save_backups/TIMESTAMP/, keeping the 5 newest. Never throws. */
    public static String backupOnLaunch(Context ctx) {
        try {
            File[] saves = listSaves(dataDir(ctx));
            if (saves.length == 0) {
                Log.i(TAG, "VrSaves: no saves to back up");
                return "No saves to back up";
            }
            File root = backupRoot(ctx);
            File dest = copyAll(saves, root);
            prune(root);
            Log.i(TAG, "VrSaves: backed up " + saves.length + " file(s) to " + dest);
            return dest.getAbsolutePath();
        } catch (Throwable t) {
            Log.e(TAG, "VrSaves.backupOnLaunch failed", t);
            return "ERROR: " + t.getMessage();
        }
    }

    private static boolean hasAllFilesAccess() {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager();
    }

    /** Copies VMU saves to /sdcard/Download/ToyCommanderVR/Saves/TIMESTAMP/. Returns folder path or "ERROR: ...". Never throws. */
    public static String exportToDownload(Context ctx) {
        try {
            if (!hasAllFilesAccess())
                return "ERROR: All files access not granted (Settings > Apps > Toy Commander VR > Permissions)";
            File[] saves = listSaves(dataDir(ctx));
            if (saves.length == 0) return "ERROR: no saves found to export";
            File dest = copyAll(saves, new File(EXPORT_ROOT));
            Log.i(TAG, "VrSaves: exported " + saves.length + " file(s) to " + dest);
            return dest.getAbsolutePath();
        } catch (Throwable t) {
            Log.e(TAG, "VrSaves.exportToDownload failed", t);
            return "ERROR: " + t.getMessage();
        }
    }

    /**
     * Restores the newest folder of /sdcard/Download/ToyCommanderVR/Saves/ over the current saves
     * (current saves are backed up first). Call ONLY while the game is not running; restart
     * emulation afterwards. Never throws.
     */
    public static String importLatestFromDownload(Context ctx) {
        try {
            if (!hasAllFilesAccess())
                return "ERROR: All files access not granted (Settings > Apps > Toy Commander VR > Permissions)";
            File[] dirs = sortedDirs(new File(EXPORT_ROOT));
            File src = null;
            for (int i = dirs.length - 1; i >= 0 && src == null; i--)
                if (listSaves(dirs[i]).length > 0) src = dirs[i];
            if (src == null) return "ERROR: no save folder found in " + EXPORT_ROOT;
            backupOnLaunch(ctx);
            File dd = dataDir(ctx);
            if (!dd.mkdirs() && !dd.isDirectory()) return "ERROR: cannot create " + dd;
            File[] files = listSaves(src);
            for (File f : files) copy(f, new File(dd, f.getName()));
            Log.i(TAG, "VrSaves: imported " + files.length + " file(s) from " + src);
            return "Imported " + files.length + " save file(s) from " + src.getAbsolutePath();
        } catch (Throwable t) {
            Log.e(TAG, "VrSaves.importLatestFromDownload failed", t);
            return "ERROR: " + t.getMessage();
        }
    }

    /**
     * At launch (before the emulator opens the saves): when the player put an empty file named
     * "import.request" in /sdcard/Download/ToyCommanderVR/Saves/ (SideQuest, a file manager),
     * the newest save folder there is imported and the request deleted. Never throws.
     */
    public static String importIfRequested(Context ctx) {
        try {
            File request = new File(EXPORT_ROOT, "import.request");
            if (!hasAllFilesAccess() || !request.isFile())
                return null;
            String result = importLatestFromDownload(ctx);
            if (!request.delete())
                Log.w(TAG, "VrSaves: could not delete " + request);
            return result;
        } catch (Throwable t) {
            Log.e(TAG, "VrSaves.importIfRequested failed", t);
            return "ERROR: " + t.getMessage();
        }
    }
}
