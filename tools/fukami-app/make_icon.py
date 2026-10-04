#!/usr/bin/env python3
"""Draw the Fukami app icon (our own artwork: no game art, logos or marks)
and write a macOS .iconset folder for iconutil.

Usage: python3 tools/fukami-app/make_icon.py OUTPUT.iconset
"""
from __future__ import annotations

from pathlib import Path
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

SIZE = 1024
FONTS = ('/System/Library/Fonts/Supplemental/Arial Black.ttf', '/System/Library/Fonts/SFNS.ttf', '/System/Library/Fonts/HelveticaNeue.ttc',
         '/System/Library/Fonts/Helvetica.ttc')


def gradient(size: int, top: tuple[int, int, int], bottom: tuple[int, int, int]) -> Image.Image:
    image = Image.new('RGB', (size, size))
    draw = ImageDraw.Draw(image)
    for y in range(size):
        t = y / (size - 1)
        draw.line([(0, y), (size, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)))
    return image


def font(size: int) -> ImageFont.FreeTypeFont:
    for path in FONTS:
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    return ImageFont.load_default()


def draw_master() -> Image.Image:
    # macOS icon grid: an 824-pixel rounded square centred on a 1024 canvas.
    canvas = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
    inset, radius = 100, 185
    box = (inset, inset, SIZE - inset, SIZE - inset)

    shadow = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((box[0], box[1] + 14, box[2], box[3] + 14), radius, fill=(0, 0, 0, 110))
    canvas.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(18)))

    mask = Image.new('L', (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).rounded_rectangle(box, radius, fill=255)
    body = gradient(SIZE, (18, 22, 48), (58, 18, 70)).convert('RGBA')

    art = ImageDraw.Draw(body)
    # Speed lines sweeping in from the left.
    for i, (y, length, width) in enumerate(((420, 330, 26), (500, 400, 30), (580, 290, 24))):
        colour = (255, 140 - i * 25, 40)
        art.rounded_rectangle((160, y, 160 + length, y + width), width // 2, fill=colour)
    # A bold, slanted F.
    letter = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
    ImageDraw.Draw(letter).text((500, 505), 'F', font=font(500), fill=(255, 255, 255, 255), anchor='mm')
    letter = letter.transform((SIZE, SIZE), Image.AFFINE, (1, 0.22, -110, 0, 1, 0), Image.BICUBIC)
    body.alpha_composite(letter)

    canvas.paste(body, (0, 0), mask)
    return canvas


def main() -> int:
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    master = draw_master()
    for base in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            pixels = base * scale
            name = f'icon_{base}x{base}{"@2x" if scale == 2 else ""}.png'
            master.resize((pixels, pixels), Image.LANCZOS).save(out / name)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
