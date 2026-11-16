# Reviewing the HD textures

The VR build upscales each small texture once, as the game first shows it, and keeps the result
in a cache on the headset (`vr.UpscaleCache`, VR menu → Image quality → HD textures; see
`core/rend/CustomTexture.cpp`). Most textures look better, but an upscale can go wrong: text
and HUD digits blurred or smeared, thin lines broken, transparent edges with halos, patterns
that were meant to be pixel-sharp. This is how to find those and send the game back to the
original texture for them.

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
3. **Review** the page in a browser. Click **Reject** on every texture that looks worse. The
   top box lists the rejected hashes; **Copy rejected list** puts them on the clipboard. Save them
   to a text file (one hash per line).
4. **Send the list to the headset:**

   ```bash
   bash tcvr/tools/texcache-review.sh reject rejected-list.txt
   ```

   It merges the list with the headset's `rejected.txt`, deletes those HD files, and keeps the
   merged list in the hub.
5. **Restart the game.** The log says `upscale cache: N textures in ... (M rejected in review)`;
   the rejected textures use their originals and are never upscaled again.

To undo a rejection, remove its line from the hub's `rejected.txt` and run step 4 with an empty
list; the texture is upscaled again the next time the game shows it.

## Rules

- The cache and the review page are made from the player's own disc: they stay on the headset
  and in the private hub. Never commit them, attach them to a PR or publish them.
- Report what kind of texture fails (text, HUD, alpha edges...) in the PR or the backlog, so the
  upscaler can learn to skip that kind on its own (e.g. by format, size or alpha), instead of
  growing the list by hand.
