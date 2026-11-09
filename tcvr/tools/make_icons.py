"""
tcvr: builds the app icon and banner of the "vr" build from the Toy Commander title screen.

    python -I tcvr/tools/make_icons.py <title-screen.jpg>

Keeps only the main logo ("toy COMMANDER" with its camouflage bar), cut out from the title's blue
background by colour, and puts it on a new blue radial gradient with a white centre. Writes
shell/android-studio/flycast/src/vr/res/drawable-*/ic_launcher.png and drawable-xhdpi/ic_banner.png
(the vr build type's resources override main's), plus tcvr/assets/icon_512.png as the master.

The logo is Sega's artwork (see README: not affiliated); replace the source to change the icon.
"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[2]
RES = ROOT / 'shell' / 'android-studio' / 'flycast' / 'src' / 'vr' / 'res'

# the logo on the 640x480 title screen, without the TM, the copyright line and the SEGA badge
LOGO_BOX = (62, 68, 578, 316)
TM_BOX = (436, 244, 470, 268)		# the TM mark, inside LOGO_BOX: cleared


def cut_logo(title: Image.Image) -> Image.Image:
    logo = title.convert('RGB').crop(LOGO_BOX)
    w, h = logo.size
    alpha = Image.new('L', (w, h), 0)
    px = logo.load()
    a = alpha.load()
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            # the title's background is blue (to white near the middle): blue clearly above red and
            # green, or a pale blue-white haze; the logo is black outline, white, red and camouflage
            blue = b > r + 35 and b > g + 15
            haze = b > 200 and r > 150 and g > 150 and b > r + 12
            a[x, y] = 0 if (blue or haze) else 255
    tx0, ty0, tx1, ty1 = (TM_BOX[0] - LOGO_BOX[0], TM_BOX[1] - LOGO_BOX[1], TM_BOX[2] - LOGO_BOX[0], TM_BOX[3] - LOGO_BOX[1])
    ImageDraw.Draw(alpha).rectangle((tx0, ty0, tx1, ty1), fill=0)
    # clean edges: drop isolated specks, soften by half a pixel
    alpha = alpha.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.GaussianBlur(0.6))
    logo.putalpha(alpha)
    return logo


def background(w: int, h: int) -> Image.Image:
    """A blue radial gradient, white in the centre."""
    bg = Image.new('RGB', (w, h))
    px = bg.load()
    cx, cy = w / 2, h / 2
    rmax = (cx * cx + cy * cy) ** 0.5
    white, light, deep = (255, 255, 255), (90, 140, 255), (8, 28, 150)
    for y in range(h):
        for x in range(w):
            t = (((x - cx) ** 2 + (y - cy) ** 2) ** 0.5) / rmax
            # white out to 30% of the radius (a glow around the logo), light blue at 62%, deep blue
            if t < 0.30:
                c = white
            elif t < 0.62:
                k = (t - 0.30) / 0.32
                c = tuple(int(white[i] + (light[i] - white[i]) * k) for i in range(3))
            else:
                k = min((t - 0.62) / 0.38, 1.0)
                c = tuple(int(light[i] + (deep[i] - light[i]) * k) for i in range(3))
            px[x, y] = c
    return bg


def compose(logo: Image.Image, w: int, h: int, fill: float) -> Image.Image:
    img = background(w, h)
    scale = min(w * fill / logo.width, h * fill / logo.height)
    lw, lh = max(1, int(logo.width * scale)), max(1, int(logo.height * scale))
    small = logo.resize((lw, lh), Image.LANCZOS)
    img.paste(small, ((w - lw) // 2, (h - lh) // 2), small)
    return img


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    logo = cut_logo(Image.open(sys.argv[1]))
    master = compose(logo, 512, 512, 0.92)
    (ROOT / 'tcvr' / 'assets').mkdir(parents=True, exist_ok=True)
    master.save(ROOT / 'tcvr' / 'assets' / 'icon_512.png')
    for folder, size in (('ldpi', 36), ('mdpi', 48), ('hdpi', 72), ('xhdpi', 96), ('xxhdpi', 144), ('xxxhdpi', 192)):
        d = RES / f'drawable-{folder}'
        d.mkdir(parents=True, exist_ok=True)
        master.resize((size, size), Image.LANCZOS).save(d / 'ic_launcher.png')
    banner = compose(logo, 640, 360, 0.86).resize((320, 180), Image.LANCZOS)
    banner.save(RES / 'drawable-xhdpi' / 'ic_banner.png')
    print('icons written to', RES)


if __name__ == '__main__':
    main()
