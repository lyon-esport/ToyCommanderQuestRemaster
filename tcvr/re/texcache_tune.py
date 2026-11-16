"""Tune HD textures as asked in the review (tcvr/docs/TextureReview.md).

    python tcvr/re/texcache_tune.py <requests.txt> <cache> <cache-orig> <out folder> [--sheet before_after.png]

<requests.txt> is the list the review page copies: one texture per line, its hash then
sharp=<-2..3> (smoother below 0, sharper above), relief=<0..3> (more local contrast: walls, stone,
wood), crisp (clean borders: shapes and lettering) or original (the game's own texture, no HD).
Example:  66183f22 sharp=-0.5 relief=2

For each line it writes <hash>.png into <out folder>, stored bottom-up like the cache (the game
loads it flipped), from the HD copy (or the original for "original"), and a before/after sheet.
The result goes to the headset's Download/ToyCommanderVR/Textures/<game id>/ (texcache-review.sh tune),
which the game reads ahead of the HD cache. Same filters as the review page's live preview.
"""
import argparse
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageEnhance, ImageFilter

CHOICES = ('crisp', 'original')
SHARP_K = (0.45, 0.6)	# per step of sharpness: towards blur (below 0), away from it (above 0)


def blend(a, b, t):
    """a + (b - a) * t, any t (above 1 extrapolates: sharpening), clipped."""
    return Image.blend(a, b, t)


def tune(orig, hd, choices, sharp=0.0, relief=0.0):
    """orig, hd: RGBA pictures (upright). sharp: -2 (smoother) .. 3 (sharper), 0 = HD as made;
    relief: 0 .. 3 (local contrast). Returns the tuned picture at the HD size. The review page's
    preview does the same steps (box blurs, scaled to the shown size)."""
    if 'original' in choices:
        # the original, scaled to the HD size with nearest pixels (the game filters it as before)
        return orig.resize(hd.size, Image.NEAREST)
    alpha = hd.getchannel('A')
    rgb = hd.convert('RGB')
    if 'crisp' in choices:
        # pixel-exact shapes from the original blended into the smooth HD, then hard alpha edges
        near = orig.resize(hd.size, Image.NEAREST)
        rgb = Image.blend(rgb, near.convert('RGB'), 0.65)
        alpha = Image.blend(alpha, near.getchannel('A'), 0.65).point(lambda a: 255 if a >= 128 else 0)
    if sharp:
        # sharpness: the picture against its 2-pixel blur, pulled towards it or pushed away
        blurred = rgb.filter(ImageFilter.BoxBlur(2))
        rgb = blend(blurred, rgb, 1 + sharp * (SHARP_K[1] if sharp > 0 else SHARP_K[0]))
    if relief:
        # relief: the same against a wide (8-pixel) blur: big shapes stand out, fine detail stays as is
        wide = rgb.filter(ImageFilter.BoxBlur(8))
        rgb = blend(wide, rgb, 1 + relief * 0.35)
        rgb = ImageEnhance.Contrast(rgb).enhance(1 + relief * 0.06)
    rgb.putalpha(alpha)
    return rgb


def parse(path):
    """Lines: <hash> [crisp] [original] [sharp=<-2..3>] [relief=<0..3>]."""
    requests = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        parts = line.replace(',', ' ').split()
        if not parts or parts[0].startswith('#'):
            continue
        h = parts[0].lower()
        if not all(ch in '0123456789abcdef' for ch in h):
            continue
        req = {'choices': [], 'sharp': 0.0, 'relief': 0.0}
        for p in parts[1:]:
            p = p.lower()
            if p in CHOICES:
                req['choices'].append(p)
            elif p.startswith(('sharp=', 'relief=')):
                key, _, val = p.partition('=')
                try:
                    req[key] = max(-2.0, min(3.0, float(val)))
                except ValueError:
                    pass
        if req['choices'] or req['sharp'] or req['relief']:
            requests[h.zfill(8)] = req
    return requests


def describe(req):
    parts = list(req['choices'])
    if req['sharp']:
        parts.append(f"sharp {req['sharp']:+g}")
    if req['relief']:
        parts.append(f"relief {req['relief']:g}")
    return ' + '.join(parts)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('requests')
    ap.add_argument('cache')
    ap.add_argument('orig')
    ap.add_argument('out')
    ap.add_argument('--sheet', default=None)
    args = ap.parse_args()
    requests = parse(args.requests)
    os.makedirs(args.out, exist_ok=True)
    done = []
    for h, req in sorted(requests.items()):
        hd_path, orig_path = os.path.join(args.cache, h + '.png'), os.path.join(args.orig, h + '.png')
        if not os.path.isfile(hd_path) or not os.path.isfile(orig_path):
            print(f'{h}: not in the cache (or no original saved), skipped')
            continue
        hd = Image.open(hd_path).convert('RGBA').transpose(Image.FLIP_TOP_BOTTOM)
        orig = Image.open(orig_path).convert('RGBA').transpose(Image.FLIP_TOP_BOTTOM)
        tuned = tune(orig, hd, req['choices'], req['sharp'], req['relief'])
        tuned.transpose(Image.FLIP_TOP_BOTTOM).save(os.path.join(args.out, h + '.png'), optimize=True)
        done.append((h, describe(req), orig, hd, tuned))
        print(f'{h}: {describe(req)}')
    if args.sheet and done:
        cell = 170
        sheet = Image.new('RGB', (3 * (cell + 6) + 14, len(done) * (cell + 24) + 30), (24, 24, 27))
        d = ImageDraw.Draw(sheet)
        d.text((10, 8), 'original | automatic HD | tuned', fill=(230, 230, 230))
        for i, (h, what, orig, hd, tuned) in enumerate(done):
            y = 26 + i * (cell + 24)
            for j, (img, rs) in enumerate(((orig, Image.NEAREST), (hd, Image.BILINEAR), (tuned, Image.BILINEAR))):
                board = Image.new('RGBA', (cell, cell), (160, 160, 160, 255))
                k = cell / max(img.size)
                board.alpha_composite(img.resize((max(1, round(img.size[0] * k)), max(1, round(img.size[1] * k))), rs))
                sheet.paste(board.convert('RGB'), (10 + j * (cell + 6), y))
            d.text((10, y + cell + 4), f'{h}: {what}', fill=(230, 200, 90))
        sheet.save(args.sheet)
        print('sheet:', args.sheet)
    print(f'{len(done)} tuned textures in {args.out}')


if __name__ == '__main__':
    sys.exit(main())
