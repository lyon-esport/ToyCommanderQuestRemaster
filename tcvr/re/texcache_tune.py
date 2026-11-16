"""Tune HD textures as asked in the review (tcvr/docs/TextureReview.md).

    python tcvr/re/texcache_tune.py <requests.txt> <cache> <cache-orig> <out folder> [--sheet before_after.png]

<requests.txt> is the list the review page copies: one texture per line, its hash then its
choices among: crisp (clean borders, less blur: shapes and lettering), sharp (sharper details),
relief (more local contrast: walls, stone, wood), original (the game's own texture, no HD).
Example:  cf7fc39e crisp

For each line it writes <hash>.png into <out folder>, stored bottom-up like the cache (the game
loads it flipped), from the HD copy (or the original for "original"), and a before/after sheet.
The result goes to the headset's Download/ToyCommanderVR/Textures/<game id>/ (texcache-review.sh tune),
which the game reads ahead of the HD cache. Same filters as the review page's live preview.
"""
import argparse
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageEnhance, ImageFilter

CHOICES = ('crisp', 'sharp', 'relief', 'original')


def tune(orig, hd, choices):
    """orig, hd: RGBA pictures (upright). Returns the tuned picture at the HD size."""
    if 'original' in choices:
        # the original, scaled to the HD size with nearest pixels (the game filters it as before)
        return orig.resize(hd.size, Image.NEAREST)
    out = hd
    alpha = out.getchannel('A')
    rgb = out.convert('RGB')
    if 'crisp' in choices:
        # pixel-exact shapes from the original blended into the smooth HD, then hard alpha edges
        near = orig.resize(hd.size, Image.NEAREST)
        rgb = Image.blend(rgb, near.convert('RGB'), 0.65).filter(ImageFilter.UnsharpMask(radius=1, percent=60, threshold=4))
        alpha = Image.blend(alpha, near.getchannel('A'), 0.65).point(lambda a: 255 if a >= 128 else 0)
    if 'sharp' in choices:
        rgb = rgb.filter(ImageFilter.UnsharpMask(radius=2, percent=130, threshold=2))
    if 'relief' in choices:
        rgb = rgb.filter(ImageFilter.UnsharpMask(radius=8, percent=70, threshold=0))
        rgb = ImageEnhance.Contrast(rgb).enhance(1.15)
    rgb.putalpha(alpha)
    return rgb


def parse(path):
    requests = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        parts = line.replace(',', ' ').split()
        if not parts or parts[0].startswith('#'):
            continue
        h = parts[0].lower()
        choices = [c.lower() for c in parts[1:] if c.lower() in CHOICES]
        if all(ch in '0123456789abcdef' for ch in h) and choices:
            requests[h.zfill(8)] = choices
    return requests


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
    for h, choices in sorted(requests.items()):
        hd_path, orig_path = os.path.join(args.cache, h + '.png'), os.path.join(args.orig, h + '.png')
        if not os.path.isfile(hd_path) or not os.path.isfile(orig_path):
            print(f'{h}: not in the cache (or no original saved), skipped')
            continue
        hd = Image.open(hd_path).convert('RGBA').transpose(Image.FLIP_TOP_BOTTOM)
        orig = Image.open(orig_path).convert('RGBA').transpose(Image.FLIP_TOP_BOTTOM)
        tuned = tune(orig, hd, choices)
        tuned.transpose(Image.FLIP_TOP_BOTTOM).save(os.path.join(args.out, h + '.png'), optimize=True)
        done.append((h, choices, orig, hd, tuned))
        print(f'{h}: {" + ".join(choices)}')
    if args.sheet and done:
        cell = 170
        sheet = Image.new('RGB', (3 * (cell + 6) + 14, len(done) * (cell + 24) + 30), (24, 24, 27))
        d = ImageDraw.Draw(sheet)
        d.text((10, 8), 'original | automatic HD | tuned', fill=(230, 230, 230))
        for i, (h, choices, orig, hd, tuned) in enumerate(done):
            y = 26 + i * (cell + 24)
            for j, (img, rs) in enumerate(((orig, Image.NEAREST), (hd, Image.BILINEAR), (tuned, Image.BILINEAR))):
                board = Image.new('RGBA', (cell, cell), (160, 160, 160, 255))
                k = cell / max(img.size)
                board.alpha_composite(img.resize((max(1, round(img.size[0] * k)), max(1, round(img.size[1] * k))), rs))
                sheet.paste(board.convert('RGB'), (10 + j * (cell + 6), y))
            d.text((10, y + cell + 4), f'{h}: {" + ".join(choices)}', fill=(230, 200, 90))
        sheet.save(args.sheet)
        print('sheet:', args.sheet)
    print(f'{len(done)} tuned textures in {args.out}')


if __name__ == '__main__':
    sys.exit(main())
