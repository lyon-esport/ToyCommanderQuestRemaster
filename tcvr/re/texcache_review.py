"""Review the HD texture cache (vr.UpscaleCache) made on the headset: a gallery to spot bad upscales.

    python tcvr/re/texcache_review.py <cache folder> [--out <review folder>] [--orig <originals folder>]

<cache folder> is a copy of the headset's texcache/v2/<game id>/ and the originals folder its
sibling <game id>-orig/ (tcvr/tools/texcache-review.sh pulls both). The review folder gets one
self-contained index.html: for every cached texture, the real original next to the HD one at the
same size, a "Reject" toggle, and a difference score: the HD picture shrunk back to the
original's size (box filter) compared with the original, as the mean colour difference and the
share of pixels that changed a lot. An upscale that keeps the picture scores near 0; one that
lost or invented details (merged lines, smeared text, changed shapes) scores high. Cards are
sorted worst first, and those over --threshold are marked "check".

The page's "Copy rejected list" button gives the hashes for rejected.txt
(tcvr/docs/TextureReview.md). The images come from the player's own disc: keep the review
folder private (never commit or publish it).
"""
import argparse
import base64
import html
import io
import os
import sys

from PIL import Image, ImageChops, ImageStat

VIEW = 160  # pixels per side shown in the page


def checker(size):
    board = Image.new('RGBA', size, (200, 200, 200, 255))
    for y in range(0, size[1], 8):
        for x in range((y // 8 % 2) * 8, size[0], 16):
            board.paste((150, 150, 150, 255), (x, y, x + 8, y + 8))
    return board


def shown(img, size, resample):
    board = checker(size)
    board.alpha_composite(img.resize(size, resample))
    return board.convert('RGB')


def uri(img):
    buf = io.BytesIO()
    img.save(buf, 'PNG', optimize=True)
    return 'data:image/png;base64,' + base64.b64encode(buf.getvalue()).decode()


def score(orig, hd):
    """(mean difference 0-255, % of pixels off by more than 48) between orig and hd shrunk back."""
    back = hd.resize(orig.size, Image.BOX)
    # compared on a grey background, so transparent areas count by what shows
    a = Image.new('RGBA', orig.size, (128, 128, 128, 255))
    b = Image.new('RGBA', orig.size, (128, 128, 128, 255))
    a.alpha_composite(orig)
    b.alpha_composite(back)
    diff = ImageChops.difference(a.convert('RGB'), b.convert('RGB'))
    mean = sum(ImageStat.Stat(diff).mean) / 3
    big = sum(1 for v in diff.convert('L').getdata() if v > 48) * 100.0 / (orig.size[0] * orig.size[1])
    return mean, big


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cache')
    ap.add_argument('--out', default=None)
    ap.add_argument('--orig', default=None, help='originals folder (default: <cache>-orig next to the cache)')
    ap.add_argument('--threshold', type=float, default=6.0, help='mean difference marked "check"')
    args = ap.parse_args()
    cache = args.cache.rstrip('/\\')
    orig_dir = args.orig or cache + '-orig'
    out = args.out or os.path.join(os.path.dirname(cache), 'review')
    os.makedirs(out, exist_ok=True)
    rejected = set()
    rej_file = os.path.join(cache, 'rejected.txt')
    if os.path.isfile(rej_file):
        rejected = {l.strip().lower() for l in open(rej_file) if l.strip() and not l.startswith('#')}
    cards = []
    missing = 0
    for name in sorted(n for n in os.listdir(cache) if n.lower().endswith('.png')):
        h = name[:-4].lower()
        # (the cache stores pictures bottom-up, as the renderer loads them: shown upright here)
        hd = Image.open(os.path.join(cache, name)).convert('RGBA').transpose(Image.FLIP_TOP_BOTTOM)
        op = os.path.join(orig_dir, name)
        real = os.path.isfile(op)
        if real:
            orig = Image.open(op).convert('RGBA').transpose(Image.FLIP_TOP_BOTTOM)
        else:
            # an older cache without originals: shrunk back from the HD copy (can't show lost detail)
            missing += 1
            f = 4 if hd.size[0] % 4 == 0 and hd.size[1] % 4 == 0 and min(hd.size) >= 32 else 2
            orig = hd.resize((hd.size[0] // f, hd.size[1] // f), Image.NEAREST)
        mean, big = score(orig, hd)
        k = VIEW / max(hd.size)
        size = (max(1, round(hd.size[0] * k)), max(1, round(hd.size[1] * k)))
        cards.append(dict(h=h, ow=orig.size[0], oh=orig.size[1], w=hd.size[0], ht=hd.size[1], mean=mean, big=big,
                          bad=h in rejected, real=real,
                          uo=uri(shown(orig, size, Image.NEAREST)), uh=uri(shown(hd, size, Image.BILINEAR))))
    cards.sort(key=lambda c: -c['mean'])
    flagged = sum(1 for c in cards if c['mean'] >= args.threshold)
    note = (f' <b class="warn">{missing} without a saved original (older cache): their left picture is rebuilt '
            'from the HD one and their score means little.</b>') if missing else ''
    with open(os.path.join(out, 'index.html'), 'w', encoding='utf-8') as page:
        page.write('''<!doctype html><meta charset="utf-8"><title>HD texture review</title>
<style>body{font:14px sans-serif;background:#18181b;color:#e4e4e7;margin:16px}
.grid{display:flex;flex-wrap:wrap;gap:12px}.t{background:#27272a;padding:8px;border-radius:6px;border:2px solid transparent}
.t.check{border-color:#f59e0b}.t.bad{border-color:#ef4444}.t img{display:block;image-rendering:pixelated}
.pair{display:flex;gap:6px}.cap{font-size:12px;color:#a1a1aa;margin-top:4px}.warn{color:#f59e0b}
button{margin-top:6px;width:100%}#bar{position:sticky;top:0;background:#18181b;padding:8px 0;z-index:1}
textarea{width:100%;height:70px}</style>
''')
        page.write(f'''<div id="bar"><b>HD texture review</b> - {len(cards)} textures, biggest difference first;
{flagged} marked <span class="warn">check</span> (difference &ge; {args.threshold:g}).{note}
Left: original, right: HD. Click "Reject" on any that look wrong.
<button onclick="copyList()" style="width:auto">Copy rejected list</button> <span id="count"></span>
<textarea id="list" readonly placeholder="rejected hashes appear here"></textarea></div><div class="grid">
''')
        for c in cards:
            cls = 't' + (' bad' if c['bad'] else '') + (' check' if c['mean'] >= args.threshold else '')
            page.write(f'''<div class="{cls}" id="t{c['h']}"><div class="pair">
<img src="{c['uo']}" title="original {c['ow']}x{c['oh']}"><img src="{c['uh']}" title="HD {c['w']}x{c['ht']}"></div>
<div class="cap">{html.escape(c['h'])} - {c['ow']}x{c['oh']} to {c['w']}x{c['ht']}<br>
difference {c['mean']:.1f}, {c['big']:.1f}% pixels changed{'' if c['real'] else ' (no original saved)'}</div>
<button onclick="toggle('{c['h']}')">Reject</button></div>
''')
        page.write('''</div><script>
function toggle(h){document.getElementById('t'+h).classList.toggle('bad');update()}
function update(){const l=[...document.querySelectorAll('.t.bad')].map(e=>e.id.slice(1));
document.getElementById('list').value=l.join('\\n');document.getElementById('count').textContent=l.length+' rejected'}
function copyList(){update();navigator.clipboard.writeText(document.getElementById('list').value)}
update()</script>
''')
    print(f'{len(cards)} textures ({missing} without an original), {flagged} to check, '
          f'{len(rejected)} already rejected -> {os.path.join(out, "index.html")}')


if __name__ == '__main__':
    sys.exit(main())
