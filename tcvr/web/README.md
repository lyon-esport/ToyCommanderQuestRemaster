# Toy Commander in a web browser

Work in progress (plan: `Browser-version-feasibility.md` in the project files). Goal: browser
players on a LAN join the same matches as Quest players, with nothing installed; a local server
serves the page, the emulator and the disc image.

Current shape (step 3 of the plan): our fork built as a **libretro core for EmulatorJS**
(WebAssembly + WebGL 2), with the SH4 to WebAssembly JIT from
[nasomers/flycast-wasm](https://github.com/nasomers/flycast-wasm) (GPLv2) in `core/rec-wasm/`.
The Toy Commander patches (60 Hz, 60 game pictures/s, unlock all) are in the portable core and come
along; the headset code is left out.

## Files

| File | What |
|---|---|
| `build.sh` | Builds the core and its EmulatorJS package `tcvr-wasm.data` |
| `gl_override.js` | `glGetString` for WebGL (from flycast-wasm, fixed: `GL_EXTENSIONS` lists the WebGL extensions; the original made RetroArch's GL init fail with "Invalid enum") |
| `patches/emulatorjs-cache-large-files.patch` | EmulatorJS fix: the 500+ MB disc read back from one Blob failed at random in Chromium (NotReadableError) |
| `page/index.html` | Test page (EmulatorJS): speed counter from the emulator's own emulated-frame count, scripted 4-pad route to a 4-player battle, log forwarding |
| `serve.py` | Local server with the COOP/COEP headers, serves the disc from a local path, collects results, and runs the lobby and the network bridge for the pages |
| `netbridge.py` | The page's UDP (GGPO netplay) through the server, over a WebSocket (`/net`) |
| `lobby.py` | A browser player in the app's LAN lobby (`core/network/tcvr_lobby.h`, joiner side), for `play.html` over `/lobby/...`; `test_lobby.py` tests it |
| `page/play.html` | The player's page: the game, and a Multiplayer panel that lists the games hosted on the LAN by headsets and PCs, joins one, and starts the match with the others |

## Pinned versions

- Emscripten SDK 3.1.74 (its zlib port may need to be put in `upstream/emscripten/cache/ports/zlib` by hand when GitHub archive downloads are blocked)
- EmulatorJS 4.2.3 (commit 5a65bde) for the page, with the patch above
- EmulatorJS RetroArch (commit 97f2cfa), built with
  `emmake make -f Makefile.emulatorjs LIBRETRO=flycast HAVE_OPENGLES3=1 HAVE_AL=1`
- flycast-wasm (commit 16e9c7b) for `build/flycast_stubs.o`

## Build

```
EJS_RA=<EmulatorJS RetroArch tree> STUBS=<flycast-wasm>/build/flycast_stubs.o tcvr/web/build.sh
```

The link refuses undefined symbols on purpose: the libretro build takes its settings from
`shell/libretro/option.cpp`, and an option missing there (our `vr.*` options were) linked as a silent
null and broke the boot.

## Test page layout

```
www/index.html            page/index.html
www/data/                 EmulatorJS data/ (patched cache.js)
www/data/cores/tcvr-wasm.data, reports/tcvr.json   from build.sh
serve.py                  python serve.py --disc <your own disc image>
```

Open `http://localhost:8642/?core=fork&script=battle`. `?script=none` leaves the pads to you,
`?elf=1` loads `/disc/test.elf` instead of the disc (checks without the game). The disc image is
never in git and never on a public server.

## Playing with headsets and PCs on the LAN

`python serve.py --disc <your own disc image> --host 0.0.0.0`, then each browser player opens
`http://<server PC address>:8642/play.html`. A headset or a PC creates the game (Multiplayer, Create
game); it shows up in the page's Multiplayer panel, Join, Ready, and the host starts the match. The page
then loads again as the match: the host's match start state, the same automatic setup as on the
other copies, GGPO through the server. Back to the lobby with the button under the panel.

Every copy in a match must be built from the same commit: the host refuses another app version
(`git describe --tags`, so build from a full clone with the tags, not a shallow one).

## Known gaps (next steps)

- Netplay is compiled out of libretro builds (`core/network/ggpo.cpp`); step 5 enables it for the
  browser and routes it through the local server (browsers have no UDP).
- LAN play needs HTTPS (secure context for SharedArrayBuffer and gamepads): step 4 server.
- The libretro build has no ImGui: the browser multiplayer menu will be HTML in the page.

## Player profiles

A browser player types a 3-letter name on the start screen. The game's save card (VMU A1, kept by the core in
`/data/saves` with the `reicast_per_content_vmus = VMU A1` option) is loaded from and saved to the local server
(`GET/POST /profile?name=ABC`, files in `profiles/ABC.json` next to `serve.py`) every 15 s and when the page is
left, so a player who comes back gets their saves from any browser or device. Matches use a blank card in memory
on every player's copy, so only solo play uses the profile. Two players who pick the same name share one profile.
