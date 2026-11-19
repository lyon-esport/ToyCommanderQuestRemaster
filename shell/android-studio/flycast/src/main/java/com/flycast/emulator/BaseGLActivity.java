// hotd2-vr: modified in 2026 by mikermak for the Quest VR mode (see "git log master..hotd2-vr").
package com.flycast.emulator;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.preference.PreferenceManager;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.ViewConfiguration;
import android.view.Window;
import android.view.WindowManager;
import android.window.OnBackInvokedDispatcher;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.core.app.ActivityCompat;

import com.flycast.emulator.config.Config;
import com.flycast.emulator.emu.AudioBackend;
import com.flycast.emulator.emu.HttpClient;
import com.flycast.emulator.emu.JNIdc;
import com.flycast.emulator.periph.InputDeviceManager;
import com.flycast.emulator.periph.SipEmulator;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

import static android.content.res.Configuration.HARDKEYBOARDHIDDEN_NO;
import static android.view.View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR;
import static android.view.WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS;
import static android.view.WindowManager.LayoutParams.FLAG_TRANSLUCENT_NAVIGATION;
import static android.view.WindowManager.LayoutParams.FLAG_TRANSLUCENT_STATUS;

public abstract class BaseGLActivity extends Activity implements ActivityCompat.OnRequestPermissionsResultCallback {
    private static final int STORAGE_PERM_REQUEST = 1001;
    private static final int AUDIO_PERM_REQUEST = 1002;

    protected SharedPreferences prefs;
    private AudioBackend audioBackend;
    private AudioManager vrAudioManager;
    private AudioFocusRequest vrAudioFocusRequest;
    private boolean vrAudioFocusGranted;
    protected Handler handler = new Handler();
    private boolean audioPermissionRequested = false;
    private boolean storagePermissionGranted = false;
    private boolean paused = true;
    private boolean resumedCalled = false;
    private String pendingIntentUrl;
    private boolean hasKeyboard = false;
    private AndroidStorage storage;

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        if (!(getApplicationContext() instanceof Emulator)) {
            // app is in restricted mode, kill it
            // See https://medium.com/@pablobaxter/what-happened-to-my-subclass-android-application-924c91bafcac
            handler.post(() -> System.exit(0));
            finish();
            return;
        }
        // Set the navigation bar color to 0 to avoid left over when it fades out on Android 10
        Window window = getWindow();
        window.addFlags(FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS);
        window.clearFlags(FLAG_TRANSLUCENT_STATUS | FLAG_TRANSLUCENT_NAVIGATION);
        window.setNavigationBarColor(0);
        window.getDecorView().setSystemUiVisibility(SYSTEM_UI_FLAG_LIGHT_STATUS_BAR);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }

        if (!getFilesDir().exists()) {
            getFilesDir().mkdir();
        }
        prefs = PreferenceManager.getDefaultSharedPreferences(this);

        Emulator.setCurrentActivity(this);
        new HttpClient().nativeInit();

        String homeDir = prefs.getString(Config.pref_home, "");
        // Check that home dir is valid
        if (homeDir.isEmpty()) {
            // home dir not set: use default
            homeDir = getDefaultHomeDir();
            prefs.edit().putString(Config.pref_home, homeDir).apply();
        }

        // tcvr: back up VMU saves once per launch (before the emulator opens them)
        try { VrSaves.backupOnLaunch(this); } catch (Throwable t) { Log.e("tcvr", "save backup failed", t); }
        // tcvr: Download/ToyCommanderVR/Saves/import.request -> import the newest save folder there
        try { VrSaves.importIfRequested(this); } catch (Throwable t) { Log.e("tcvr", "save import failed", t); }

        String result = JNIdc.initEnvironment((Emulator)getApplicationContext(), getFilesDir().getAbsolutePath(), homeDir,
                Locale.getDefault().toString());
        if (result != null) {
            showAlertDialog(result);
            return;
        }
        Log.i("flycast", "Environment initialized");
        Emulator app = (Emulator)getApplicationContext();
        app.getConfigurationPrefs();
        storage = new AndroidStorage(this);
        setStorageDirectories();

        boolean externalStorageLegacy = true;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            externalStorageLegacy = Environment.isExternalStorageLegacy();
            //Log.i("flycast", "External storage legacy: " + (externalStorageLegacy ? "preserved" : "lost"));
        }
        if (!storagePermissionGranted) {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M || !externalStorageLegacy)
                // No permission needed before Android 6
                // Permissions only needed in legacy external storage mode
                storagePermissionGranted = true;
            else {
                Log.i("flycast", "Asking for external storage permission");
                ActivityCompat.requestPermissions(this,
                        new String[]{
                                Manifest.permission.READ_EXTERNAL_STORAGE,
                                Manifest.permission.WRITE_EXTERNAL_STORAGE
                        },
                        STORAGE_PERM_REQUEST);
            }
        }

        Log.i("flycast", "Initializing input devices");
        InputDeviceManager.getInstance().startListening(getApplicationContext());
        register(this);
        audioBackend = new AudioBackend();
        onConfigurationChanged(getResources().getConfiguration());

        // Back button handling on Android 13+
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT, () -> {
                onBackKey();
            });
        }

        // When viewing a resource, pass its URI to the native code for opening
        Intent intent = getIntent();
        // hotd2-vr: settings for this run from the launch (adb shell am start ... --es
        // hotd2vr.config "network:GGPO=yes,..."), as -config on the PC; a test aid
        String launchConfig = intent != null ? intent.getStringExtra("hotd2vr.config") : null;
        if (launchConfig != null && !launchConfig.isEmpty())
            JNIdc.setConfigOverrides(launchConfig);
        if (Intent.ACTION_VIEW.equals(intent.getAction())) {
            Uri gameUri = intent.getData();
            // Flush the intent to prevent multiple calls
            getIntent().setData(null);
            setIntent(null);
            if (gameUri != null) {
                if (storagePermissionGranted)
                    JNIdc.setGameUri(gameUri.toString());
                else
                    pendingIntentUrl = gameUri.toString();
            }
        }
        // tcvr: the vr build is now com.dreamcast4ever.toycommandervr (no ".vr" suffix any more)
        else if (getPackageName().endsWith(".vr") || getPackageName().equals("com.dreamcast4ever.toycommandervr")) {
            // hotd2-vr: started from the headset's app library. The Flycast menu can't be
            // shown in the headset, so boot the player's game directly, or show the setup
            // panel when it can't be found (or read) yet.
            if (vrBootedGame != null) {
                // tcvr: another instance of this activity in a process that runs the game already
                // (started again while its task was there, e.g. "am start"): it takes over the running
                // emulator (its surface). Booting the game again ended a multiplayer match.
                vrGamePath = vrBootedGame;
                Log.i("flycast", "VR build started again, the game runs already: not booted again");
                Log.i("flycast", "BaseGLActivity.onCreate done");
                return;
            }
            String game = findVrGame();
            vrGamePath = game;
            Log.i("flycast", "VR build started without a game, booting " + game);
            if (game != null)
                vrBootedGame = game;
            if (game == null) {
                showVrSetup();
                return;
            }
            if (storagePermissionGranted)
                JNIdc.setGameUri(game);
            else
                pendingIntentUrl = game;
        }
        Log.i("flycast", "BaseGLActivity.onCreate done");
    }

    // tcvr: the game again from a fresh process after the saves changed on disk (the emulator keeps
    // the saves open): the setup panel, in a process of its own, ends this one and starts the game
    // (as its Play button does). A delayed relaunch from here is refused by Android (background start).
    private String restartForSaves(Context ctx, String done)
    {
        VrGames.Scan scan = VrGames.scan(ctx);
        if (scan.game == null)
            return done + ". Quit and start the game again to use them.";
        Intent relaunch = new Intent(ctx, VrSetupActivity.class);
        relaunch.putExtra(VrSetupActivity.EXTRA_RELAUNCH, scan.game.getAbsolutePath());
        relaunch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        handler.postDelayed(() -> startActivity(relaunch), 300);
        return done + ". Restarting...";
    }

    // tcvr: called from native code (the VR menu, core/rend/vr/xr_menu.h), on the render thread:
    // 0 export the saves to Download, 1 import the newest exported saves and start the game
    // again, 2 quit, 3 unlock all levels (then start the game again), 4 the first saved game's player
    // name ("" if none), 5 start the game again (a multiplayer match begins or ends that way, with
    // its options already in emu.cfg). Returns the line the menu shows.
    public String onVrMenuRequest(int request)
    {
        Context ctx = getApplicationContext();
        switch (request)
        {
        case 0: {
            String result = VrSaves.exportToDownload(ctx);
            if (result.startsWith("ERROR"))
                return result.substring(Math.min(result.length(), 7));
            int download = result.indexOf("/Download/");
            return "Saves exported to " + (download >= 0 ? result.substring(download + 1) : result);
        }
        case 1: {
            String result = VrSaves.importLatestFromDownload(ctx);
            if (result.startsWith("ERROR"))
                return result.substring(Math.min(result.length(), 7));
            Log.i("flycast", "tcvr: " + result + ", restarting");
            return restartForSaves(ctx, "Saves imported");
        }
        case 3: {
            // "Unlock all levels": patches the saved games on disk, then a fresh game reads them
            // (the running game keeps its own copy of the progress in RAM)
            String result = VrSaves.unlockAllLevels(ctx);
            if (result.startsWith("ERROR"))
                return result.substring(Math.min(result.length(), 7));
            Log.i("flycast", "tcvr: " + result + ", restarting");
            return restartForSaves(ctx, result);
        }
        case 4:
            return VrSaves.firstPlayerName(ctx);
        case 5:
            Log.i("flycast", "tcvr: multiplayer, restarting the game");
            return restartForSaves(ctx, "Restarting the game");
        case 2:
            Log.i("flycast", "tcvr: quit from the VR menu");
            handler.post(() -> {
                finishAndRemoveTask();
                handler.postDelayed(() -> android.os.Process.killProcess(android.os.Process.myPid()), 500);
            });
            return "Quitting...";
        default:
            return "Unknown request";
        }
    }

	// Called from native code
    public void showAlertDialog(String message)
    {
        handler.post(new Runnable() {
            @Override
            public void run() {
                AlertDialog.Builder dlgAlert = new AlertDialog.Builder(BaseGLActivity.this);
                dlgAlert.setMessage(getResources().getString(R.string.init_failed_message) + message);
                dlgAlert.setTitle(R.string.flycast_error);
                dlgAlert.setPositiveButton(R.string.exit,
                        new DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(DialogInterface dialog, int id) {
                                BaseGLActivity.this.finish();
                            }
                        });
                dlgAlert.setIcon(android.R.drawable.ic_dialog_alert);
                dlgAlert.setCancelable(false);
                dlgAlert.create().show();
            }
        });
    }

    // tcvr: the disc image started from the headset's library (null otherwise)
    private String vrGamePath;
    // tcvr: the disc image this process booted (VR build), for the whole process: a later instance of
    // the activity does not boot it again (a new game starts in a new process: Quit, the setup panel)
    private static String vrBootedGame;

    // tcvr: called from native code (emulator.cpp) when the disc isn't Toy Commander (Europe), the
    // only version the VR build runs: kept as refused, and back to the setup panel, which says why.
    // The emulator's process ends: its load failed, and nothing of it can be shown in the headset.
    public void onVrUnsupportedGame(String gameId)
    {
        Log.w("flycast", "tcvr: unsupported disc " + vrGamePath + " (game ID [" + gameId + "])");
        if (vrGamePath != null)
            VrGames.rememberRefused(getApplicationContext(), new File(vrGamePath), gameId);
        handler.post(() -> {
            showVrSetup();
            handler.postDelayed(() -> android.os.Process.killProcess(android.os.Process.myPid()), 1000);
        });
    }

    // hotd2-vr: the game to start: the one picked on the setup panel, or the only one there is
    // (null: the setup panel, to find one or to pick one)
    private String findVrGame()
    {
        Intent intent = getIntent();
        String picked = intent != null ? intent.getStringExtra(VrSetupActivity.EXTRA_GAME) : null;
        if (picked != null && new File(picked).isFile())
            return picked;
        VrGames.Scan scan = VrGames.scan(this);
        return scan.games.size() == 1 ? scan.game.getAbsolutePath() : null;
    }

    // hotd2-vr: the setup panel, in the headset's home (an immersive app opens a panel that way)
    private void showVrSetup()
    {
        Intent panel = new Intent(getApplicationContext(), VrSetupActivity.class);
        panel.setAction(Intent.ACTION_MAIN);
        panel.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            android.app.PendingIntent pending = android.app.PendingIntent.getActivity(getApplicationContext(), 0, panel,
                    android.app.PendingIntent.FLAG_UPDATE_CURRENT | android.app.PendingIntent.FLAG_IMMUTABLE);
            Intent home = new Intent(Intent.ACTION_MAIN)
                    .addCategory(Intent.CATEGORY_HOME)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    .putExtra("extra_launch_in_home_pending_intent", pending);
            startActivity(home);
        } catch (Exception e) {
            Log.w("flycast", "Setup panel through the home failed, opening it directly", e);
            startActivity(panel);
        }
        finish();
    }

    private void setStorageDirectories()
    {
        String android_home_directory = Environment.getExternalStorageDirectory().getAbsolutePath();
        List<String> pathList = new ArrayList<>();
        pathList.add(android_home_directory);
        pathList.addAll(FileBrowser.getExternalMounts());
        pathList.add(getApplicationContext().getFilesDir().getAbsolutePath());
        File dir= getApplicationContext().getExternalFilesDir(null);
        if (dir != null)
            pathList.add(dir.getAbsolutePath());
        Log.i("flycast", "Storage dirs: " + pathList);
        if (storage != null)
            storage.setStorageDirectories(pathList);
        JNIdc.setExternalStorageDirectories(pathList.toArray());
    }

    // Testing
    public AndroidStorage getStorage() {
        return storage;
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        paused = true;
        InputDeviceManager.getInstance().stopListening();
        register(null);
        if (audioBackend != null)
            audioBackend.release();
        Emulator.setCurrentActivity(null);
        stopEmulator();
    }

    @Override
    protected void onPause() {
        super.onPause();
        resumedCalled = false;
        handleStateChange(true);
        releaseVrAudioFocus();
    }

    @Override
    protected void onResume() {
        super.onResume();
        resumedCalled = true;
        requestVrAudioFocus();
        handleStateChange(false);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            requestVrAudioFocus();
            handleStateChange(false);
        } else {
            handleStateChange(true);
        }
    }

    private void requestVrAudioFocus() {
        if (!resumedCalled || vrAudioFocusGranted
                || !(getPackageName().endsWith(".vr") || getPackageName().equals("com.dreamcast4ever.toycommandervr")))
            return;
        if (vrAudioManager == null)
            vrAudioManager = (AudioManager)getSystemService(AUDIO_SERVICE);
        if (vrAudioManager == null)
            return;
        if (vrAudioFocusRequest == null) {
            vrAudioFocusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_GAME)
                            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                            .build())
                    .setOnAudioFocusChangeListener(change -> {
                        vrAudioFocusGranted = change == AudioManager.AUDIOFOCUS_GAIN;
                        Log.i("flycast", "VR audio focus changed: " + change);
                    }, handler)
                    .build();
        }
        int result = vrAudioManager.requestAudioFocus(vrAudioFocusRequest);
        vrAudioFocusGranted = result == AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
        Log.i("flycast", "VR audio focus request: " + result);
    }

    private void releaseVrAudioFocus() {
        if (vrAudioManager != null && vrAudioFocusRequest != null)
            vrAudioManager.abandonAudioFocusRequest(vrAudioFocusRequest);
        vrAudioFocusGranted = false;
    }

    @Override
    public void onConfigurationChanged(@NonNull Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        hasKeyboard = newConfig.hardKeyboardHidden == HARDKEYBOARDHIDDEN_NO;
    }

    protected abstract void doPause();
    protected abstract void doResume();
    protected abstract boolean isSurfaceReady();

    public void handleStateChange(boolean paused)
    {
        if (paused == this.paused)
            return;
        if (!paused && (!resumedCalled || !isSurfaceReady()))
            return;
        this.paused = paused;
        if (paused) {
            doPause();
        }
        else {
            doResume();
        }
    }

    private boolean showMenu() {
        JNIdc.guiOpenSettings();
        return true;
    }

    private boolean processJoystickInput(MotionEvent event, InputDevice.MotionRange range) {
        float v = event.getAxisValue(range.getAxis(), event.getActionIndex());
        // normalize to [-32768, 32767]
        v = (v - range.getMin()) / range.getRange() * 65535.f - 32768.f;
        return InputDeviceManager.getInstance().axisEvent(event.getDeviceId(), range.getAxis(), (int)Math.round(v));
    }
    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        InputDeviceManager deviceManager = InputDeviceManager.getInstance();
        if ((event.getSource() & InputDevice.SOURCE_CLASS_JOYSTICK) == InputDevice.SOURCE_CLASS_JOYSTICK
                && event.getAction() == MotionEvent.ACTION_MOVE
                && event.getDevice() != null)
        {
            List<InputDevice.MotionRange> axes = event.getDevice().getMotionRanges();
            boolean rc = false;
            int actionPointerIndex = event.getActionIndex();
            for (InputDevice.MotionRange range : axes)
            {
                if ((range.getSource() & InputDevice.SOURCE_CLASS_MASK) != InputDevice.SOURCE_CLASS_JOYSTICK)
		            // Ignore mouse/touchpad axes (dualshock 4)
		            continue;
                if (range.getAxis() == MotionEvent.AXIS_HAT_X) {
                    float v = event.getAxisValue(MotionEvent.AXIS_HAT_X, actionPointerIndex);
                    if (v == -1.0) {
                        rc |= deviceManager.buttonEvent(event.getDeviceId(), KeyEvent.KEYCODE_DPAD_LEFT, true);
                        deviceManager.buttonEvent(event.getDeviceId(), KeyEvent.KEYCODE_DPAD_RIGHT, false);
                    }
                    else if (v == 1.0) {
                        deviceManager.buttonEvent(event.getDeviceId(), KeyEvent.KEYCODE_DPAD_LEFT, false);
                        rc |= deviceManager.buttonEvent(event.getDeviceId(), KeyEvent.KEYCODE_DPAD_RIGHT, true);
                    } else {
                        deviceManager.buttonEvent(event.getDeviceId(), KeyEvent.KEYCODE_DPAD_LEFT, false);
                        deviceManager.buttonEvent(event.getDeviceId(), KeyEvent.KEYCODE_DPAD_RIGHT, false);
                    }
                }
                else if (range.getAxis() == MotionEvent.AXIS_HAT_Y) {
                    float v = event.getAxisValue(MotionEvent.AXIS_HAT_Y, actionPointerIndex);
                    if (v == -1.0) {
                        rc |= deviceManager.buttonEvent(event.getDeviceId(), KeyEvent.KEYCODE_DPAD_UP, true);
                        deviceManager.buttonEvent(event.getDeviceId(), KeyEvent.KEYCODE_DPAD_DOWN, false);
                    }
                    else if (v == 1.0) {
                        deviceManager.buttonEvent(event.getDeviceId(), KeyEvent.KEYCODE_DPAD_UP, false);
                        rc |= deviceManager.buttonEvent(event.getDeviceId(), KeyEvent.KEYCODE_DPAD_DOWN, true);
                    } else {
                        deviceManager.buttonEvent(event.getDeviceId(), KeyEvent.KEYCODE_DPAD_UP, false);
                        deviceManager.buttonEvent(event.getDeviceId(), KeyEvent.KEYCODE_DPAD_DOWN, false);
                    }
                }
                else {
                    rc |= processJoystickInput(event, range);
                }
            }
            if (rc)
                return true;
        }
        else if ((event.getSource() & InputDevice.SOURCE_CLASS_POINTER) == InputDevice.SOURCE_CLASS_POINTER) {
            switch (event.getAction()) {
                case MotionEvent.ACTION_SCROLL:
                    deviceManager.mouseScrollEvent(Math.round(-event.getAxisValue(MotionEvent.AXIS_VSCROLL)));
                    break;
                default:
                    deviceManager.mouseEvent(Math.round(event.getX()), Math.round(event.getY()), event.getButtonState());
                    break;
            }
            return true;
        }
        return super.onGenericMotionEvent(event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (InputDeviceManager.getInstance().buttonEvent(event.getDeviceId(), keyCode, false))
            return true;
        if (hasKeyboard && InputDeviceManager.getInstance().keyboardEvent(keyCode, false))
            return true;
        return super.onKeyUp(keyCode, event);
    }

    private void onBackKey() {
        if (JNIdc.guiIsContentBrowser()) {
            finish();
        }
        else {
            showMenu();
        }
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (event.getRepeatCount() == 0) {
            if (keyCode == KeyEvent.KEYCODE_BACK) {
                onBackKey();
                return true;
            }
            InputDeviceManager deviceManager = InputDeviceManager.getInstance();
            if (deviceManager.buttonEvent(event.getDeviceId(), keyCode, true))
                return true;

            if (hasKeyboard) {
                deviceManager.keyboardEvent(keyCode, true);
                if (!event.isCtrlPressed() && (event.isPrintingKey() || event.getKeyCode() == KeyEvent.KEYCODE_SPACE))
                    deviceManager.keyboardText(event.getUnicodeChar());
                return true;
            }
            if (ViewConfiguration.get(this).hasPermanentMenuKey()) {
                if (keyCode == KeyEvent.KEYCODE_MENU) {
                    return showMenu();
                }
            }
        }
        return super.onKeyDown(keyCode, event);
    }

    protected void stopEmulator() {
        JNIdc.stop();
    }

    void requestRecordAudioPermission() {
        if (audioPermissionRequested)
            return;
        audioPermissionRequested = true;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            handler.post(new Runnable() {
                @Override
                public void run() {
                    Log.i("flycast", "Requesting Record audio permission");
                    ActivityCompat.requestPermissions(BaseGLActivity.this,
                            new String[]{
                                    Manifest.permission.RECORD_AUDIO
                            },
                            AUDIO_PERM_REQUEST);

                }
            });
        }
        else
        {
            onRequestPermissionsResult(AUDIO_PERM_REQUEST, new String[] { Manifest.permission.RECORD_AUDIO },
                    new int[] { PackageManager.PERMISSION_GRANTED });
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M)
            super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == AUDIO_PERM_REQUEST && permissions.length > 0
                && Manifest.permission.RECORD_AUDIO .equals(permissions[0]) && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
            Log.i("flycast", "Record audio permission granted");
            SipEmulator sip = new SipEmulator();
            JNIdc.setupMic(sip);
        }
        else if (requestCode == STORAGE_PERM_REQUEST) {
            Log.i("flycast", "External storage permission granted");
            storagePermissionGranted = true;
            setStorageDirectories();
            if (pendingIntentUrl != null) {
                JNIdc.setGameUri(pendingIntentUrl);
                pendingIntentUrl = null;
            }

            //setup mic
            if (InputDeviceManager.isMicPluggedIn())
                requestRecordAudioPermission();
        }

    }

    private String getDefaultHomeDir() {
        File dir = getExternalFilesDir(null);
        if (dir == null)
            dir = getFilesDir();
        return dir.getAbsolutePath();
    }

    // Called from native code
    public void onGameStateChange(boolean started) {
        runOnUiThread(new Runnable() {
            public void run() {
                if (started)
                    getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                else
                    getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            }
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        switch (requestCode)
        {
            case AndroidStorage.ADD_STORAGE_ACTIVITY_REQUEST:
                storage.onAddStorageResult(data);
                break;
            case AndroidStorage.IMPORT_HOME_ACTIVITY_REQUEST:
                storage.onImportHomeResult(data);
                break;
            case AndroidStorage.EXPORT_HOME_ACTIVITY_REQUEST:
                storage.onExportHomeResult(data);
                break;
        }
    }

    private static native void register(BaseGLActivity activity);
    
	public String getNativeLibDir() {
        return getApplicationContext().getApplicationInfo().nativeLibraryDir;
    }
    
	public String getInternalFilesDir() {
        return getFilesDir().getAbsolutePath();
    }
}
