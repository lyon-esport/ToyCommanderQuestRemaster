// hotd2-vr: added in 2026 by mikermak for the Quest VR mode (see "git log master..hotd2-vr").
package com.flycast.emulator;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.ActivityNotFoundException;
import android.content.ComponentName;
import android.content.Intent;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Process;
import android.os.SystemClock;
import android.provider.Settings;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.util.List;

/**
 * hotd2-vr: the setup panel. Shown in the headset's home when the game can't start yet: no
 * access to the files, or no game found, or more than one game to choose from. It says what's
 * missing, step by step, and starts the game in VR once it's all there.
 */
public class VrSetupActivity extends Activity
{
    /** The game picked here, for the VR activity (a path). */
    public static final String EXTRA_GAME = "hotd2vr.game";
    /** tcvr: start this game again at once (the VR menu's save import), from this panel's own process. */
    public static final String EXTRA_RELAUNCH = "tcvr.relaunch";

    private static final int RED = Color.rgb(217, 32, 26), TEXT = Color.rgb(230, 230, 230),
            SOFT = Color.rgb(160, 160, 164), GOOD = Color.rgb(96, 200, 110), BACK = Color.rgb(18, 18, 20);

    private TextView accessStatus, gameStatus;
    private Button allow;
    private LinearLayout games;

    @Override
    protected void onCreate(Bundle savedInstanceState)
    {
        super.onCreate(savedInstanceState);
        // tcvr: a restart asked by the game (VR menu, saves imported): end its process, start it again
        String relaunch = getIntent() != null ? getIntent().getStringExtra(EXTRA_RELAUNCH) : null;
        if (relaunch != null && new File(relaunch).isFile())
        {
            play(new File(relaunch));
            return;
        }
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(36), dp(28), dp(36), dp(28));
        page.setBackgroundColor(BACK);

        TextView title = text("TOY COMMANDER VR", 28, RED);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        page.addView(title);
        page.addView(text("Two steps before the first game.", 17, SOFT), spaced(4));

        page.addView(heading("1.  Let the app read your files"), spaced(24));
        page.addView(accessStatus = text("", 16, SOFT), spaced(6));
        allow = button("Allow");
        allow.setOnClickListener(v -> askForAccess());
        page.addView(allow, spaced(10));

        page.addView(heading("2.  Put your Toy Commander disc image on the headset"), spaced(24));
        page.addView(gameStatus = text("", 16, SOFT), spaced(6));
        Button again = button("Look again");
        again.setOnClickListener(v -> refresh());
        page.addView(again, spaced(10));

        // a Play button per game found
        games = new LinearLayout(this);
        games.setOrientation(LinearLayout.VERTICAL);
        page.addView(games, spaced(20));

        // tcvr: how to open the VR menu in the game (Manu), with a picture of the button
        page.addView(heading("In the game: the VR menu"), spaced(28));
        page.addView(text("Hold the menu button of the left controller (the small flat button under the stick), "
                + "or press it twice. A short press is the game's Start. The VR menu has recenter, world size, "
                + "comfort and image settings, save export and import, and quit.", 16, SOFT), spaced(6));
        page.addView(new ControllerHint(this), new LinearLayout.LayoutParams(dp(470), dp(170)));

        page.addView(text("Bring your own copy of Toy Commander (Dreamcast disc image). Nothing of the "
                + "game comes with this app. Unofficial fan project, not affiliated with Sega or No Cliche.", 13, SOFT), spaced(22));

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(BACK);
        scroll.addView(page);
        setContentView(scroll);
    }

    @Override
    protected void onResume()
    {
        super.onResume();
        refresh();
    }

    private void refresh()
    {
        boolean access = VrGames.canReadSharedStorage();
        VrGames.Scan scan = VrGames.scan(this);
        boolean ready = !scan.games.isEmpty();
        if (access)
        {
            accessStatus.setText("Done.");
            accessStatus.setTextColor(GOOD);
            allow.setVisibility(View.GONE);
        }
        else
        {
            accessStatus.setText("The app looks for your Toy Commander disc image in the headset's Download folder. For that it needs "
                    + "\"All files access\": tap Allow, switch it on, and come back here.");
            accessStatus.setTextColor(SOFT);
            allow.setVisibility(View.VISIBLE);
        }
        if (ready)
        {
            StringBuilder found = new StringBuilder(scan.games.size() == 1 ? "Found: " : "Found, pick one below:");
            for (VrGames.Game g : scan.games)
                found.append(scan.games.size() == 1 ? "" : "\n").append(shortPath(g.file));
            String wrong = wrongVersionText(scan);
            if (wrong != null)
                found.append("\n\n").append(wrong);
            gameStatus.setText(found);
            gameStatus.setTextColor(GOOD);
        }
        else
        {
            String how = "Copy your Toy Commander disc image to the headset's Download folder (a subfolder such as Download/ToyCommanderVR is fine): a .chd, a .gdi or a .cue with its "
                    + ".bin files, or a .cdi. With SideQuest (Manage files), or connect the headset to a PC "
                    + "with USB, allow access in the headset, and open Quest > Internal shared storage > Download.";
            if (scan.incomplete != null)
                how = "Found " + shortPath(scan.incomplete) + ", but not the files it lists next to it: "
                        + TextUtils.join(", ", scan.missing) + ". Copy those to the same folder.";
            else if (!scan.wrongVersion.isEmpty())
                how = wrongVersionText(scan);
            else if (scan.archive != null)
                how = "Found " + shortPath(scan.archive) + ": that's packed. Unpack it first (on a PC), then "
                        + "copy what comes out of it to the Download folder.";
            else if (!access)
                how += " Then allow the access above.";
            gameStatus.setText(how);
            gameStatus.setTextColor(scan.incomplete != null || scan.archive != null || !scan.wrongVersion.isEmpty() ? RED : SOFT);
        }
        games.removeAllViews();
        for (VrGames.Game g : scan.games)
        {
            Button play = button("Play " + g.title);
            play.setOnClickListener(v -> play(g.file));
            games.addView(play, spaced(8));
        }
    }

    /** tcvr: the disc images that are another version than Toy Commander (Europe), and what to do; null: none. */
    private String wrongVersionText(VrGames.Scan scan)
    {
        if (scan.wrongVersion.isEmpty())
            return null;
        StringBuilder s = new StringBuilder();
        for (VrGames.Game g : scan.wrongVersion)
            s.append("Found ").append(shortPath(g.file)).append(", but it isn't the European Toy Commander disc (its game ID is ")
                    .append(g.gameId.isEmpty() ? "missing" : g.gameId).append(").\n");
        s.append("Toy Commander VR only runs Toy Commander (Europe) (En,Fr,De,Es), game ID ").append(VrGames.SUPPORTED_GAME_ID)
                .append(": every VR setting is made for that version. Copy that disc image to the Download folder.");
        return s.toString();
    }

    private void askForAccess()
    {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R)
            return;
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
        } catch (ActivityNotFoundException e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    private void play(File game)
    {
        // A game may still be running in the app's own process (the app opened again from the
        // library while one played): the emulator has one game per process, and its task would
        // only be brought back. So that process ends, and the game picked starts in a new one.
        endGameProcess();
        // the app's launcher entry, which starts in VR
        Intent intent = new Intent(Intent.ACTION_MAIN);
        intent.setComponent(new ComponentName(this, "com.flycast.emulator.MainActivity"));
        intent.putExtra(EXTRA_GAME, game.getAbsolutePath());
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(intent);
        finish();
    }

    /** Ends the app's main process (this panel runs in one of its own), and waits for it. */
    private void endGameProcess()
    {
        ActivityManager am = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
        List<ActivityManager.RunningAppProcessInfo> running = am != null ? am.getRunningAppProcesses() : null;
        if (running == null)
            return;
        for (ActivityManager.RunningAppProcessInfo p : running)
        {
            if (p.pid == Process.myPid() || !getPackageName().equals(p.processName))
                continue;
            Process.killProcess(p.pid);
            for (int i = 0; i < 50 && new File("/proc/" + p.pid).exists(); i++)
                SystemClock.sleep(20);
        }
    }

    private String shortPath(File f)
    {
        String path = f.getAbsolutePath();
        String root = Environment.getExternalStorageDirectory().getAbsolutePath();
        return path.startsWith(root + "/") ? path.substring(root.length() + 1) : f.getName();
    }

    private TextView text(String s, int sp, int colour)
    {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(colour);
        t.setLineSpacing(0, 1.15f);
        return t;
    }

    private TextView heading(String s)
    {
        TextView t = text(s, 20, TEXT);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private Button button(String s)
    {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 17);
        b.setTextColor(Color.WHITE);
        b.setGravity(Gravity.CENTER);
        b.setPadding(dp(28), dp(8), dp(28), dp(8));
        GradientDrawable shape = new GradientDrawable();
        shape.setColor(RED);
        shape.setCornerRadius(dp(8));
        b.setBackground(shape);
        return b;
    }

    private LinearLayout.LayoutParams spaced(int top)
    {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        p.topMargin = dp(top);
        return p;
    }

    private int dp(int v)
    {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    /** tcvr: the left controller seen from above, its menu button lit (the same picture as the loading panel's). */
    private static class ControllerHint extends View
    {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

        ControllerHint(Context context)
        {
            super(context);
        }

        @Override
        protected void onDraw(Canvas c)
        {
            final float sz = getHeight(), cx = sz * 0.45f, top = 0f;
            final int body = Color.rgb(77, 77, 84), dark = Color.rgb(31, 31, 33), mid = Color.rgb(133, 133, 140),
                    glow = Color.rgb(255, 217, 77);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(body);
            c.drawOval(cx - sz * 0.30f, top + sz * 0.05f, cx + sz * 0.30f, top + sz * 0.89f, paint);
            paint.setColor(dark);
            c.drawCircle(cx - sz * 0.06f, top + sz * 0.30f, sz * 0.10f, paint);		// the stick
            paint.setColor(mid);
            c.drawCircle(cx - sz * 0.06f, top + sz * 0.30f, sz * 0.065f, paint);
            c.drawCircle(cx + sz * 0.12f, top + sz * 0.36f, sz * 0.055f, paint);	// Y
            c.drawCircle(cx + sz * 0.16f, top + sz * 0.52f, sz * 0.055f, paint);	// X
            paint.setColor(dark);
            paint.setTextAlign(Paint.Align.CENTER);
            paint.setFakeBoldText(true);
            paint.setTextSize(sz * 0.08f);
            c.drawText("Y", cx + sz * 0.12f, top + sz * 0.388f, paint);
            c.drawText("X", cx + sz * 0.16f, top + sz * 0.548f, paint);
            final float bx = cx - sz * 0.07f, by = top + sz * 0.66f;				// the menu button
            paint.setColor(glow);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(sz * 0.025f);
            c.drawCircle(bx, by, sz * 0.092f, paint);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(RED);
            c.drawCircle(bx, by, sz * 0.055f, paint);
            paint.setColor(Color.WHITE);
            for (int i = -1; i <= 1; i++)
                c.drawRect(bx - sz * 0.028f, by + i * sz * 0.018f - sz * 0.005f, bx + sz * 0.028f, by + i * sz * 0.018f + sz * 0.005f, paint);
            // its label, to the right
            paint.setColor(glow);
            c.drawRect(bx + sz * 0.12f, by - sz * 0.006f, cx + sz * 0.62f, by + sz * 0.006f, paint);
            paint.setTextAlign(Paint.Align.LEFT);
            paint.setTextSize(sz * 0.13f);
            c.drawText("hold, or press twice", cx + sz * 0.66f, by + sz * 0.045f, paint);
        }
    }
}
