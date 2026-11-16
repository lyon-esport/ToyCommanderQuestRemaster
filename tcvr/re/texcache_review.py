"""Review the HD texture cache (vr.UpscaleCache) made on the headset: a gallery to spot bad upscales.

    python tcvr/re/texcache_review.py <cache folder> [--out <review folder>]

<cache folder> is a copy of the headset's texcache/<game id>/ (tcvr/tools/texcache-review.sh pulls
it). The review folder gets index.html: for every cached texture, the original (shrunk back from the
HD copy, nearest pixel) next to the HD one, both scaled up to the same size, with a "Reject" toggle.
The page's "Copy rejected list" button gives the hashes to put in rejected.txt (see
tcvr/docs/TextureReview.md): the game then uses the original texture again and never upscales it.

The images are made from the player's own disc: keep the review folder private (never commit it).
"""
import argparse
import base64
import html
import io
import os
import sys

from PIL import Image

VIEW = 160  # pixels per side shown in the page


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cache')
    ap.add_argument('--out', default=None)
    ap.add_argument('--factor', type=int, default=0, help='upscale factor of the cache (0: guess from sizes)')
    args = ap.parse_args()
    out = args.out or os.path.join(args.cache, '..', 'review')
    os.makedirs(out, exist_ok=True)
    rejected = set()
    rej_file = os.path.join(args.cache, 'rejected.txt')
    if os.path.isfile(rej_file):
        rejected = {l.strip().lower() for l in open(rej_file) if l.strip() and not l.startswith('#')}
    rows = []
    names = sorted(n for n in os.listdir(args.cache) if n.lower().endswith('.png'))
    for name in names:
        h = name[:-4].lower()
        # (the cache stores pictures bottom-up, as the renderer loads them: shown upright here)
        hd = Image.open(os.path.join(args.cache, name)).convert('RGBA').transpose(Image.FLIP_TOP_BOTTOM)
        w, ht = hd.size
        f = args.factor or (4 if w % 4 == 0 and ht % 4 == 0 and min(w, ht) >= 32 else 2)
        orig = hd.resize((max(1, w // f), max(1, ht // f)), Image.NEAREST)
        scale = VIEW / max(w, ht)
        size = (max(1, round(w * scale)), max(1, round(ht * scale)))
        # on a checkerboard, so transparency shows
        def shown(img, resample):
            img = img.resize(size, resample)
            board = Image.new('RGBA', size, (200, 200, 200, 255))
            for y in range(0, size[1], 8):
                for x in range((y // 8 % 2) * 8, size[0], 16):
                    board.paste((150, 150, 150, 255), (x, y, x + 8, y + 8))
            board.alpha_composite(img)
            return board.convert('RGB')
        # embedded in the page (one file, opens anywhere)
        def uri(img):
            buf = io.BytesIO()
            img.save(buf, 'PNG', optimize=True)
            return 'data:image/png;base64,' + base64.b64encode(buf.getvalue()).decode()
        rows.append((h, w // f, ht // f, w, ht, h in rejected, uri(shown(orig, Image.NEAREST)), uri(shown(hd, Image.BILINEAR))))
    with open(os.path.join(out, 'index.html'), 'w', encoding='utf-8') as page:
        page.write('''<!doctype html><meta charset="utf-8"><title>HD texture review</title>
<style>body{font:14px sans-serif;background:#18181b;color:#e4e4e7;margin:16px}
.grid{display:flex;flex-wrap:wrap;gap:12px}.t{background:#27272a;padding:8px;border-radius:6px;border:2px solid transparent}
.t.bad{border-color:#ef4444}.t img{display:block;image-rendering:pixelated}.pair{display:flex;gap:6px}
.cap{font-size:12px;color:#a1a1aa;margin-top:4px}button{margin-top:6px;width:100%}
#bar{position:sticky;top:0;background:#18181b;padding:8px 0;z-index:1}textarea{width:100%;height:80px}</style>
<div id="bar"><b>HD texture review</b> - %d textures. Left: original, right: HD. Click "Reject" on any that look wrong.
<button onclick="copyList()" style="width:auto">Copy rejected list</button> <span id="count"></span>
<textarea id="list" readonly placeholder="rejected hashes appear here"></textarea></div><div class="grid">
''' .replace('%d', str(len(rows))))
        for h, ow, oh, w, ht, bad, uo, uh in rows:
            page.write(f'''<div class="t{' bad' if bad else ''}" id="t{h}"><div class="pair">
<img src="{uo}" title="original {ow}x{oh}"><img src="{uh}" title="HD {w}x{ht}"></div>
<div class="cap">{html.escape(h)} - {ow}x{oh} to {w}x{ht}</div>
<button onclick="toggle('{h}')">Reject</button></div>
''')
        page.write('''</div><script>
function toggle(h){document.getElementById('t'+h).classList.toggle('bad');update()}
function update(){const l=[...document.querySelectorAll('.t.bad')].map(e=>e.id.slice(1));
document.getElementById('list').value=l.join('\\n');document.getElementById('count').textContent=l.length+' rejected'}
function copyList(){update();navigator.clipboard.writeText(document.getElementById('list').value)}
update()</script>
''')
    print(f'{len(rows)} textures, {len(rejected)} already rejected -> {os.path.join(out, "index.html")}')


if __name__ == '__main__':
    sys.exit(main())
