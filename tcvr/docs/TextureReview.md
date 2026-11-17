# Reviewing the HD textures

The VR build upscales each small texture once, as the game first shows it, and keeps the result
in a cache on the headset (`vr.UpscaleCache`, VR menu → Image quality → HD textures; see
`core/rend/CustomTexture.cpp`). Most textures look better, but an upscale can go wrong: text
and HUD digits blurred or smeared, thin lines broken, transparent edges with halos, patterns
that were meant to be pixel-sharp. This is how to find those and send the game back to the
original texture for them.

## Sizes

VR menu → Image quality → **Textures**: Original, HD 2x or HD 4x (applies at the next start). Each
size has its own cache on the headset (`texcache/v2/<game id>/` and `<game id>-x4/`), so switching
keeps both. HD 4x is for textures up to 128x128 (a bigger one would take 4 MB of video memory at
4x); bigger textures get 2x in the 4x cache. For the 4x cache, add `4` to `pull` and `tune`.

## The steps

1. **Play** the levels to review (the cache fills as textures appear).
2. **Pull and build the review page** (laptop, headset connected over ADB):

   ```bash
   bash tcvr/tools/texcache-review.sh pull
   ```

   It copies the cache to the private hub (`AI/TextureReview/<game id>/cache/` (from the headset's `texcache/v2/<game id>/`)) and writes
   `AI/TextureReview/<game id>/review/index.html`: one card per texture, the real original on the
   left (the game keeps a copy in `texcache/v2/<game id>-orig/`) and the HD one on the right, at the
   same size, with a **difference score**: the HD picture shrunk back and compared with the original
   (mean colour difference and % of pixels changed a lot). Upscales that changed or lost details
   score high; cards are sorted worst first and marked "check" over the threshold (6 by default).
3. **Review** the page in a browser. Each card shows the original, the automatic HD version and,
   once you change something, a live preview of the result. Per texture:
   - **Sharpness** slider, -2 to +3: below 0 smoother, above 0 sharper (0: the automatic HD);
   - **Relief** slider, 0 to 3: more local contrast, so walls, stone and wood look less flat. It keeps
     the softness: for "more relief but still soft", use e.g. Sharpness -1 and Relief 2;
   - **Crisp edges**: clean borders (shapes, bars, lettering, HUD pieces);
   - **Original**: no HD version at all;
   - **Reset (keep HD)**: back to the automatic HD version ("Reset all" for every texture).
   **Copy request list** puts lines like `66183f22 sharp=-1 relief=2` on the clipboard; save them
   to a text file.
4. **Make and send the tuned textures:**

   ```bash
   bash tcvr/tools/texcache-review.sh tune requests.txt
   ```

   It builds the tuned HD textures on the laptop (`tcvr/re/texcache_tune.py`, same filters as the
   preview), writes a before/after sheet (`AI/TextureReview/<game id>/tuned_sheet.png`: original,
   automatic HD, tuned), and copies them to `Download/ToyCommanderVR/Textures/<game id>/` on the
   headset. The game reads that folder ahead of its automatic HD cache.
5. **Restart the game.** The log says `N tuned textures in ...`.

To drop a texture from the HD cache entirely without tuning it, `texcache-review.sh reject <list>`
(one hash per line) still works: the game keeps the original and never upscales it again.

To start the cache over (after a fix to the upscaler, or to get originals saved for an older
cache): `bash tcvr/tools/texcache-review.sh clear`, then restart the game. It also forgets the rejected list: send the hub's list again (step 4).

To undo a rejection, clear the cache and send the hub's list again without that line (the
cache's own list is rebuilt from what is sent after a clear).

## Rules

- The cache and the review page are made from the player's own disc: they stay on the headset
  and in the private hub. Never commit them, attach them to a PR or publish them.
- Report what kind of texture fails (text, HUD, alpha edges...) in the PR or the backlog, so the
  upscaler can learn to skip that kind on its own (e.g. by format, size or alpha), instead of
  growing the list by hand.
