# Making a player build (release APK)

For maintainers. Players follow `tcvr/docs/Install.md`.

A player build is the same `vr` build type with `-Ptcvr.release`: signed with the project's own
release key (not the shared debug key) and **not debuggable**. Test builds stay debug-signed and
debuggable (needed for `run-as`, captures and the scripted test aids).

Commands are for **Windows PowerShell** on the maintainer's PC. `%USERPROFILE%` /
`$env:USERPROFILE` is your own profile folder (`C:\Users\<you>`); type the commands as written and
Windows fills it in. `$env:ANDROID_HOME` is the Android SDK folder.

## Rules for the key

- The key file and its passwords **never** go into Git, onto a shared or synced drive (Google
  Drive, the project's shared drive), into a chat, a ticket or an e-mail.
- They live in your user profile (`%USERPROFILE%\keys\`), in your password manager, and on **one
  offline backup** (a USB stick kept somewhere safe).
- Every future update must be signed with this same key. Lose it and players have to uninstall
  (losing their saves unless exported) to install the next version.

| What | Where |
|---|---|
| Keystore (the key) | `%USERPROFILE%\keys\tcvr-release.jks` |
| Signing properties (paths + passwords) | `%USERPROFILE%\keys\tcvr-signing.properties` |
| Key alias | `tcvr` |
| Certificate (public) | `CN=Crazymonster999, OU=VR LAB, O=Toy Commander VR, O=Lyon e-Sport, L=Lyon, ST=Unknown, C=FR`, created 2026-11-17 |
| Certificate SHA-256 (public) | `F9:FD:0F:5A:E6:A4:E8:7C:8B:F8:3F:3B:FA:0C:5B:02:AB:0D:74:73:88:9C:B7:28:D5:4B:93:16:BD:7B:B8:58` |
| keytool | `C:\Program Files\Android\Android Studio\jbr\bin\keytool.exe` (Android Studio's JDK; `keytool` is usually not on the PATH) |
| apksigner | `$env:ANDROID_HOME\build-tools\36.1.0\apksigner.bat` (any recent build-tools version works) |

## 1. Create the release key (once)

1. Pick **one** strong password (20+ characters) and save it in your password manager first, in a
   new entry such as "Toy Commander VR release key". The keystore uses the PKCS12 format, where the
   key password is the same as the keystore password: keytool asks for it once (twice to confirm).

2. Create the folder and the key:

   ```powershell
   New-Item -ItemType Directory -Force "$env:USERPROFILE\keys"
   & "C:\Program Files\Android\Android Studio\jbr\bin\keytool.exe" -genkeypair -v `
     -keystore "$env:USERPROFILE\keys\tcvr-release.jks" -storetype PKCS12 `
     -alias tcvr -keyalg RSA -keysize 4096 -validity 10000
   ```

   keytool asks for the password, then for a name, organisation, city, country code. These go into
   the public certificate that every player can read: a project or team name is enough (for
   example `Toy Commander VR`), no personal address needed. Answer `yes` (or `oui`) to confirm.

3. Note the certificate fingerprint (public, not secret) in the password-manager entry:

   ```powershell
   & "C:\Program Files\Android\Android Studio\jbr\bin\keytool.exe" -list -v `
     -keystore "$env:USERPROFILE\keys\tcvr-release.jks" -alias tcvr
   ```

   Copy the `SHA256:` line. It is how you (and players) recognise a genuine build later.

## 2. The signing properties file (once)

The build reads the key through a small properties file named by the environment variable
`TCVR_SIGNING_PROPERTIES` (see `shell/android-studio/flycast/build.gradle.kts`). Nothing secret is
in the repository.

1. Open it in Notepad (it creates the file):

   ```powershell
   notepad "$env:USERPROFILE\keys\tcvr-signing.properties"
   ```

2. Paste the four lines below. Replace `<you>` with your Windows user name and both
   `<password>` with the password from step 1. Use **forward slashes** `/` in the path (a
   backslash is an escape character in a properties file).

   ```properties
   storeFile=C:/Users/<you>/keys/tcvr-release.jks
   storePassword=<password>
   keyAlias=tcvr
   keyPassword=<password>
   ```

3. Save and close.

4. Tell Windows where the file is, for your user only (once; open a new PowerShell window
   afterwards for it to apply):

   ```powershell
   [Environment]::SetEnvironmentVariable('TCVR_SIGNING_PROPERTIES', "$env:USERPROFILE\keys\tcvr-signing.properties", 'User')
   ```

   This only stores the file's location, not the password. Test builds ignore it: they are signed
   with the debug key unless `-Ptcvr.release` is given.

## 3. Back up the key (once, right after step 2)

1. **Password manager:** the password and the fingerprint are already in the entry. If your
   password manager takes attachments, attach `tcvr-release.jks` too.
2. **One offline copy:** copy the folder to a USB stick and keep the stick somewhere safe (not
   with the laptop):

   ```powershell
   $usb = Read-Host 'USB stick drive letter (as shown in File Explorer)'
   Copy-Item -Recurse "$env:USERPROFILE\keys" "${usb}:\tcvr-keys-backup"
   ```

3. Check the copy opens: run the `keytool -list` command of step 1.3 with
   `-keystore "${usb}:\tcvr-keys-backup\tcvr-release.jks"` and the same password.

Do not put the backup on Google Drive, the shared drive or any synced folder.

## 4. Build a signed player APK

From the private hub folder, in PowerShell:

```powershell
.\tools\build-quest.ps1 -GradleArguments '--no-daemon','-Ptcvr.release'
```

If `TCVR_SIGNING_PROPERTIES` is missing or points to no file, the build stops with
`-Ptcvr.release needs TCVR_SIGNING_PROPERTIES` rather than producing a wrongly signed APK. A wrong
password stops it with a keystore error.

The APK is `code\shell\android-studio\flycast\build\outputs\apk\vr\flycast-vr.apk`. It overwrites
the last test build at the same place: copy it out and rename it `ToyCommanderVR-<versionName>.apk`
(the version is the build time, `YYYYMMDD_HHMMSS`). Build a test build again before using the
scripted test tools (`deploy.sh`, captures, `pad.sh` need a debuggable APK).

## 5. Check the signature

```powershell
$apk = 'ToyCommanderVR-<version>.apk'
& "$env:ANDROID_HOME\build-tools\36.1.0\apksigner.bat" verify --print-certs -v $apk
& "$env:ANDROID_HOME\build-tools\36.1.0\aapt.exe" dump badging $apk | Select-String '^package|debuggable'
```

(`apksigner.bat` needs Java: if it says `JAVA_HOME` is not set, run
`$env:JAVA_HOME = 'C:\Program Files\Android\Android Studio\jbr'` first.)

Expected:

- `Verified using v2 scheme (APK Signature Scheme v2): true`;
- `Signer #1 certificate DN:` is your release certificate, **not** `CN=Android Debug`;
- `Signer #1 certificate SHA-256 digest:` equals the fingerprint saved in step 1.3 (same hex
  digits, without the colons);
- the `package:` line shows `com.dreamcast4ever.toycommandervr`, and **no**
  `application-debuggable` line.

## 6. Publish

- GPL-2.0-or-later: publish the exact source of the build with it (a Git tag of the commit, e.g.
  `release-<version>`, on the public repository), and the licence notice (README).
- No game data in the APK or next to it; the disc image is the player's own.
- Name and icon: *Toy Commander VR*, original logo with a "VR" mark (decided 2026-11-17).
- GitHub release: tag `v<major>.<minor>.<patch>` on the built commit, attach
  `ToyCommanderVR-<version>.apk`, link `tcvr/docs/Install.md` in the notes.
- Attach `Install.md` (or link it) to the release, and optionally the certificate fingerprint.

## If the key is lost or leaked

**Lost** (no copy left anywhere, or the password is forgotten): it cannot be recovered or
recreated. Create a new key (steps 1 to 3) and tell players that the next version is a fresh
install: they export their saves (VR menu → Export saves), uninstall, install the new APK, then
import the saves. The application id can stay the same. Check the password-manager attachment
and the USB copy before concluding it is lost.

**Leaked** (the file and password may be known to someone else): someone could sign a fake
update that installs over the real one. Make a new key the same way, publish the next version
signed with it and announce its fingerprint so players can check. While the old key is still in
your hands, `apksigner rotate` (APK Signature Scheme v3.1) can link old and new keys so that
existing installs accept the update; this needs testing before use.

**Changed computer:** copy the `keys` folder from the backup to `%USERPROFILE%\keys` on the new
PC, fix the user name in `storeFile`, and redo step 2.4.

## Switching an installed headset from a test build

A test build (debug key) and a player build (release key) have the same application id but
different signatures: Android refuses to update one with the other. Export the saves first (VR
menu → Export saves), uninstall, install the player build, then import the saves.
