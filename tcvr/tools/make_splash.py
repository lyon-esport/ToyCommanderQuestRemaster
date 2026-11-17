"""Makes the Meta system splash for the VR build (shown by Horizon OS while the app starts).

    python tcvr/tools/make_splash.py
writes shell/android-studio/flycast/src/vr/assets/vr_splash.png (1024x1024, text only: the
public name and art are still to be chosen, backlog P0-7). Needs Pillow; the font is the
repo's own Roboto Bold (fonts/Roboto-Bold.ttf.zip).
"""
import io
import os
import sys
import zipfile

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, 'shell', 'android-studio', 'flycast', 'src', 'vr', 'assets', 'vr_splash.png')
TITLE = 'TOY COMMANDER VR'
LINE = 'Loading...'


def font(size):
    with zipfile.ZipFile(os.path.join(ROOT, 'fonts', 'Roboto-Bold.ttf.zip')) as z:
        name = next(n for n in z.namelist() if n.lower().endswith('.ttf'))
        return ImageFont.truetype(io.BytesIO(z.read(name)), size)


def main():
    size = 1024
    img = Image.new('RGBA', (size, size), (18, 18, 20, 255))
    d = ImageDraw.Draw(img)
    red, grey = (217, 32, 26, 255), (160, 160, 164, 255)
    # a thin red frame, like the in-headset panels
    d.rectangle([60, 340, size - 60, size - 340], outline=(158, 15, 15, 255), width=6)
    for text, f, colour, y in ((TITLE, font(78), red, 415), (LINE, font(44), grey, 540)):
        w = d.textlength(text, font=f)
        d.text(((size - w) / 2, y), text, font=f, fill=colour)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    img.save(OUT, optimize=True)
    print(OUT)


if __name__ == '__main__':
    sys.exit(main())
