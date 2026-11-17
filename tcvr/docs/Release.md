# Making a player build (release APK)

For maintainers. Players follow `tcvr/docs/Install.md`.

A player build is the same `vr` build type with `-Ptcvr.release`: signed with the project's own
release key (not the shared debug key) and **not debuggable**. Test builds stay debug-signed and
debuggable (needed for `run-as`, captures and the scripted test aids).

## 1. The release key (once, by the project owner)

Create it on a private machine, **not** in the repository and not on a shared drive, and back it
up: every update must be signed with the same key, or players have to uninstall (and lose their
saves) to update.

```bash
keytool -genkeypair -v -keystore toycommandervr-release.jks -alias toycommandervr \
  -keyalg RSA -keysize 4096 -validity 10000
```

Then a properties file next to it, for example `toycommandervr-signing.properties`:

```properties
storeFile=D:/path/to/toycommandervr-release.jks
storePassword=...
keyAlias=toycommandervr
keyPassword=...
```

Neither file is ever committed (the pre-commit hook and `.gitignore` don't know them: keep them
outside the checkout).

## 2. Build

From the private hub, in PowerShell:

```powershell
$env:TCVR_SIGNING_PROPERTIES = 'D:\path\to\toycommandervr-signing.properties'
.\tools\build-quest.ps1 -GradleArguments '--no-daemon','-Ptcvr.release'
```

Without `TCVR_SIGNING_PROPERTIES` the build stops with an error rather than producing a wrongly
signed APK. The APK is `code/shell/android-studio/flycast/build/outputs/apk/vr/flycast-vr.apk`;
rename it `ToyCommanderVR-<versionName>.apk` (the version is the build time, `YYYYMMDD_HHMMSS`).

Check it:

```bash
"$ANDROID_HOME/build-tools/36.0.0/apksigner" verify --print-certs ToyCommanderVR-<version>.apk
aapt dump badging ToyCommanderVR-<version>.apk | grep -E "package|debuggable"
```

The certificate must be the release key's, and `application-debuggable` must be absent.

## 3. Publish

- GPL-2.0-or-later: publish the exact source of the build with it (a Git tag of the commit, e.g.
  `release-<version>`, on the public repository), and the licence notice (README).
- No game data in the APK or next to it; the disc image is the player's own.
- Public name and icon: still to be decided (backlog P0-7) before any public release.
- Attach `Install.md` (or link it) to the release.

## Switching an installed headset from a test build

A test build (debug key) and a player build (release key) have the same application id but
different signatures: Android refuses to update one with the other. Export the saves first (VR
menu → Export saves), uninstall, install the player build, then import the saves.
