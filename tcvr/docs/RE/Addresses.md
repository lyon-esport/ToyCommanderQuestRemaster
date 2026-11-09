# Toy Commander (EU) — Reverse-engineering findings

Disc: Toy Commander (Europe) (En,Fr,De,Es). Main executable: `1ST_READ.BIN` (extract locally into `Docs/RE/dumps/`, which is git-ignored).

| Address | Size/type | Meaning | How verified | Date |
|---|---|---|---|---|
| — | — | — | — | — |

## Targets (in order)
1. Projection / FOV / culling frustum (to widen the view for VR)
2. Which polygon lists hold the HUD/2D overlays
3. Camera mode (free / fixed / cutscene) and camera and vehicle pose
4. FMV (Sofdec) playback state
5. Language and save structures
6. Unlock flags
