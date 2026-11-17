"""
tcvr: builds the app icon and banner of the "vr" build from the Toy Commander title screen.

    python -I tcvr/tools/make_icons.py <title-screen.jpg>
    python -I tcvr/tools/make_icons.py --from-master     (no title screen: reuses tcvr/assets/icon_512.png)

Keeps only the main logo ("toy COMMANDER" with its camouflage bar), cut out from the title's blue
background by colour, and puts it on a new blue radial gradient with a white centre. Writes
shell/android-studio/flycast/src/vr/res/drawable-nodpi/tcvr_icon.png (512x512) and tcvr_banner.png
(1280x720), plus tcvr/assets/icon_512.png as the master. The vr manifest points android:icon/banner at
them. They are density-independent on purpose: the Quest reports density 200, so a density bucket set
gave the launcher and the library (and QGO) the 72x72 hdpi icon, stretched and blurry.

The logo is Sega's artwork (see README: not affiliated); replace the source to change the icon.
"""
import sys
from pathlib import Path
from PIL import Image, ImageChops, ImageDraw, ImageFilter

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


def logo_from_master(master: Image.Image) -> Image.Image:
    """The logo back out of icon_512.png: every pixel that differs from the known background."""
    master = master.convert('RGB')
    diff = ImageChops.difference(master, background(*master.size)).convert('L')
    alpha = diff.point(lambda v: 0 if v < 3 else min(255, v * 8))
    box = alpha.getbbox()
    logo = master.crop(box)
    logo.putalpha(alpha.crop(box))
    return logo


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    master_path = ROOT / 'tcvr' / 'assets' / 'icon_512.png'
    if sys.argv[1] == '--from-master':
        logo = logo_from_master(Image.open(master_path))
    else:
        logo = cut_logo(Image.open(sys.argv[1]))
        master = compose(logo, 512, 512, 0.92)
        master_path.parent.mkdir(parents=True, exist_ok=True)
        master.save(master_path)
    out = RES / 'drawable-nodpi'
    out.mkdir(parents=True, exist_ok=True)
    compose(logo, 512, 512, 0.92).save(out / 'tcvr_icon.png', optimize=True)
    compose(logo, 1280, 720, 0.86).save(out / 'tcvr_banner.png', optimize=True)
    print('icons written to', out)


if __name__ == '__main__':
    main()
