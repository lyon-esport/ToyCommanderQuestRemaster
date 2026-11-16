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

Per texture the reviewer picks Crisp edges / Sharper / More relief / Original (live preview with
the filters of tcvr/re/texcache_tune.py); "Copy request list" gives the lines for
texcache-review.sh tune (tcvr/docs/TextureReview.md). The images come from the player's own disc: keep the review
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
.opts{display:flex;flex-wrap:wrap;gap:4px;margin-top:6px}.opts button{flex:1 1 45%;padding:3px;background:#3f3f46;color:#e4e4e7;border:1px solid #52525b;border-radius:4px;cursor:pointer}
.opts button.on{background:#2563eb;border-color:#60a5fa}.opts button.reset{flex-basis:100%;background:#27272a}.t.req{border-color:#3b82f6}canvas{display:block;image-rendering:auto}
#bar button{margin-top:6px}#bar{position:sticky;top:0;background:#18181b;padding:8px 0;z-index:1}
textarea{width:100%;height:70px}</style>
''')
        page.write(f'''<div id="bar"><b>HD texture review</b> - {len(cards)} textures, biggest difference first;
{flagged} marked <span class="warn">check</span> (difference &ge; {args.threshold:g}).{note}
Left: original, middle: automatic HD; a third picture previews your choices.
<b>Reset</b> keeps the automatic HD version. Per texture: <b>Crisp edges</b> (clean borders, less blur), <b>Sharper</b>, <b>More relief</b> (local contrast:
walls, stone, wood), <b>Original</b> (no HD). Then "Copy request list" and run
<code>texcache-review.sh tune &lt;file&gt;</code> (tcvr/docs/TextureReview.md).
<button onclick="copyList()" style="width:auto">Copy request list</button>
<button onclick="resetAll()" style="width:auto">Reset all</button> <span id="count"></span>
<textarea id="list" readonly placeholder="requests appear here: hash choice..."></textarea></div><div class="grid">
''')
        for c in cards:
            cls = 't' + (' bad' if c['bad'] else '') + (' check' if c['mean'] >= args.threshold else '')
            page.write(f'''<div class="{cls}" id="t{c['h']}"><div class="pair">
<img class="o" src="{c['uo']}" title="original {c['ow']}x{c['oh']}"><img class="h" src="{c['uh']}" title="HD {c['w']}x{c['ht']}">
<canvas class="p" title="tuned preview" style="display:none"></canvas></div>
<div class="cap">{html.escape(c['h'])} - {c['ow']}x{c['oh']} to {c['w']}x{c['ht']}<br>
difference {c['mean']:.1f}, {c['big']:.1f}% pixels changed{'' if c['real'] else ' (no original saved)'}</div>
<div class="opts" data-h="{c['h']}"><button data-c="crisp">Crisp edges</button><button data-c="sharp">Sharper</button>
<button data-c="relief">More relief</button><button data-c="original"{' class="on"' if c['bad'] else ''}>Original</button>
<button data-c="reset" class="reset">Reset (keep HD)</button></div></div>
''')
        page.write(r'''</div><script>
// the same filters as tcvr/re/texcache_tune.py, on the shown (small) pictures
function boxBlur(d,w,h,r){const o=new Float32Array(d.length),t=new Float32Array(d.length);
 for(let y=0;y<h;y++)for(let x=0;x<w;x++)for(let c=0;c<3;c++){let s=0,n=0;for(let k=-r;k<=r;k++){const xx=Math.min(w-1,Math.max(0,x+k));s+=d[(y*w+xx)*4+c];n++}t[(y*w+x)*4+c]=s/n}
 for(let y=0;y<h;y++)for(let x=0;x<w;x++)for(let c=0;c<3;c++){let s=0,n=0;for(let k=-r;k<=r;k++){const yy=Math.min(h-1,Math.max(0,y+k));s+=t[(yy*w+x)*4+c];n++}o[(y*w+x)*4+c]=s/n}
 return o}
function unsharp(img,r,amount,thr){const d=img.data,b=boxBlur(d,img.width,img.height,r);
 for(let i=0;i<d.length;i+=4)for(let c=0;c<3;c++){const diff=d[i+c]-b[i+c];if(Math.abs(diff)>=thr)d[i+c]=Math.max(0,Math.min(255,d[i+c]+diff*amount))}}
function contrast(img,k){const d=img.data;for(let i=0;i<d.length;i+=4)for(let c=0;c<3;c++)d[i+c]=Math.max(0,Math.min(255,128+(d[i+c]-128)*k))}
function render(card){const opts=[...card.querySelectorAll('.opts .on')].map(b=>b.dataset.c);
 const o=card.querySelector('img.o'),h=card.querySelector('img.h'),cv=card.querySelector('canvas.p');
 if(!opts.length){cv.style.display='none';return}
 cv.width=h.naturalWidth;cv.height=h.naturalHeight;const g=cv.getContext('2d');const W=cv.width,H=cv.height;
 if(opts.includes('original')){g.imageSmoothingEnabled=false;g.drawImage(o,0,0,W,H)}
 else{g.drawImage(h,0,0,W,H);const k=W/160;
  if(opts.includes('crisp')){g.globalAlpha=0.65;g.imageSmoothingEnabled=false;g.drawImage(o,0,0,W,H);g.globalAlpha=1;
   const im=g.getImageData(0,0,W,H);unsharp(im,Math.max(1,Math.round(k)),0.6,4);g.putImageData(im,0,0)}
  if(opts.includes('sharp')){const im=g.getImageData(0,0,W,H);unsharp(im,Math.max(1,Math.round(2*k)),1.3,2);g.putImageData(im,0,0)}
  if(opts.includes('relief')){const im=g.getImageData(0,0,W,H);unsharp(im,Math.max(2,Math.round(8*k)),0.7,0);contrast(im,1.15);g.putImageData(im,0,0)}}
 cv.style.display='block'}
function resetCard(box){box.querySelectorAll('button').forEach(x=>x.classList.remove('on'));const card=box.closest('.t');card.classList.remove('req');render(card)}
function resetAll(){document.querySelectorAll('.opts').forEach(resetCard);update()}
document.querySelectorAll('.opts button').forEach(b=>b.onclick=()=>{const box=b.parentNode;
 if(b.dataset.c==='reset'){resetCard(box);update();return}
 if(b.dataset.c==='original'){box.querySelectorAll('button').forEach(x=>{if(x!==b)x.classList.remove('on')})}
 else box.querySelector('[data-c=original]').classList.remove('on');
 b.classList.toggle('on');const card=box.closest('.t');card.classList.toggle('req',!!box.querySelector('.on'));render(card);update()});
function update(){const l=[...document.querySelectorAll('.opts')].map(x=>[x.dataset.h,...[...x.querySelectorAll('.on')].map(b=>b.dataset.c)]).filter(a=>a.length>1).map(a=>a.join(' '));
 document.getElementById('list').value=l.join('\n');document.getElementById('count').textContent=l.length+' requests'}
function copyList(){update();navigator.clipboard.writeText(document.getElementById('list').value)}
document.querySelectorAll('.opts').forEach(x=>{if(x.querySelector('.on')){const c=x.closest('.t');c.classList.add('req');render(c)}});update()</script>
''')
    print(f'{len(cards)} textures ({missing} without an original), {flagged} to check, '
          f'{len(rejected)} already rejected -> {os.path.join(out, "index.html")}')


if __name__ == '__main__':
    sys.exit(main())
