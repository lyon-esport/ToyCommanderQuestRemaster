# Decompile branch: `tcvr/decomp`

After v1.0.0, deeper VR integration and multiplayer need more than memory patches found by
RAM dumps: they need to understand the game's real code. This work happens on the long-lived
branch **`tcvr/decomp`**, started from `main` right after v1.0.0.

## What lands there

Features that depend on knowing the game's code (see [ROADMAP.md](../../ROADMAP.md#decompile-branch-tcvrdecomp)):

- the 360° head-driven camera (parked on `tcvr/camera-360`, PR #18, now based on `tcvr/decomp`);
- nicer water (the options are in the roadmap, parked until the decompile);
- VR options inside the game's own menus, which needs the menu action handlers;
- LAN multiplayer: what game state the head orientation touches, determinism, and the netplay
  inputs.

## Branch workflow

```
main ─────●──────────────────────●───────────  releases (v1.0.x fixes)
           \                      \ (merge main in regularly)
tcvr/decomp ●───●────────●─────────●───●──────  decompile-era integration
                 \      /               \
      tcvr/decomp-<topic>         back to main when a feature is ready to ship
```

- `main` stays releasable: bug fixes for v1.0.x go there as before.
- Each decompile-era feature gets its own branch named `tcvr/decomp-<topic>` (for example
  `tcvr/decomp-camera`, `tcvr/decomp-netplay`), branched from `tcvr/decomp`, with a pull request
  **into `tcvr/decomp`**.
- `main` is merged into `tcvr/decomp` regularly (a merge, never a rebase: the branch is shared).
- A feature moves to `main` with a pull request from `tcvr/decomp` (or from its topic branch)
  once it is tested on the headset and ready for a release.
- The leak scan and the game-data checks (`tcvr/tools/hooks`) apply as on `main`.

## Copyright: what never goes in this repository

The repository will become public (GPL). The game's code and data belong to their owner, so the
following are **never committed, on any branch**:

- the disc image, `1ST_READ.BIN` or any file extracted from the disc (models, textures, sounds,
  videos, text);
- RAM, VRAM or audio dumps;
- decompiled C, disassembly listings of whole functions, and the disassembler's project
  databases (Ghidra `*.gpr` / `*.rep`, IDA `*.idb` / `*.i64`, Binary Ninja `*.bndb`).

These live in the **decompile workspace**, outside this repository (see below).

What **may** be committed, because it is our own work:

- addresses, sizes and the meaning of game variables and routines (`tcvr/docs/RE/*.md`), with
  short excerpts of a few instructions where a note needs them;
- our own symbol and type names (for example a header of addresses for the patches);
- our scripts (`tcvr/re/*.py`) that read a dump given on the command line;
- the emulator-side patches and hooks that use those addresses.

`.gitignore` and the pre-commit hook block the usual file types of the list above (and a
`decomp-workspace/` folder, should one ever be created inside the checkout). They are a
safety net, not the rule: check what you stage.

## Decompile workspace

Outside the code checkout, so nothing can be committed by mistake: a `decomp/` folder in the
private hub, next to `code/`, on the team's shared drive (decided 2026-11-17). It is ignored by
the hub's own git as well, so it is never synced to any repository. Scripts and docs refer to it
as `$TCVR_DECOMP` (set in the hub's `tools/local.env`).

Close the disassembler before the shared drive syncs: a project database copied while it is
open can be corrupted.

Suggested layout:

```
$TCVR_DECOMP/
  disc/        files extracted from the disc (1ST_READ.BIN, ...)
  ghidra/      the Ghidra project (SH-4, base 0x8C010000)
  dumps/       RAM dumps by session
  export/      decompiled C exported from Ghidra, by function
  notes/       working notes that quote decompiled code
```

Findings that are our own work (addresses, meaning, how it was verified) are copied into
`tcvr/docs/RE/` in this repository, so the patches stay explained.
