"""Bakes a .ttf into a bitmap font with a smooth outline and a soft drop shadow, as PS1 games had them: each
letter is drawn once, here, and the game draws it as one textured square (two triangles) instead of outlining
it at run time.

    python tools/make_bitmap_font.py <font.ttf> <out folder> [pixel size]

Writes <out>/font_1.png, font_2.png, ... (256 x 256 each) and <out>/font.lua: the line height and, per character,
its page, place in the page, offset from the line's top-left and advance (all in pixels). Letters are white
inside and dark outside, so the game's colour tints the inside and leaves the outline dark.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

src, out = sys.argv[1], sys.argv[2]
SIZE = int(sys.argv[3]) if len(sys.argv) > 3 else 30
STROKE = max(1, round(SIZE * 0.11))             # outline thickness
SHADOW = (round(SIZE * 0.05), round(SIZE * 0.1))   # drop shadow offset
PAGE = 256
INK, OUTLINE, SHADE = (255, 255, 255, 255), (14, 12, 34, 255), (16, 22, 70, 120)

font = ImageFont.truetype(src, SIZE)
ascent, descent = font.getmetrics()
line = ascent + descent
pad = STROKE + max(SHADOW) + 1

glyphs = {}
for code in range(33, 127):
    ch = chr(code)
    w = int(font.getlength(ch)) + 2 * pad
    h = line + 2 * pad
    img = Image.new("RGBA", (w, h), OUTLINE[:3] + (0,))   # transparent pixels carry the outline colour: no fringe
    shadow = Image.new("RGBA", (w, h), OUTLINE[:3] + (0,))
    ImageDraw.Draw(shadow).text((pad + SHADOW[0], pad + SHADOW[1]), ch, font=font, fill=SHADE, stroke_width=STROKE, stroke_fill=SHADE)
    img = Image.alpha_composite(img, shadow)
    ImageDraw.Draw(img).text((pad, pad), ch, font=font, fill=INK, stroke_width=STROKE, stroke_fill=OUTLINE)
    box = img.getchannel("A").getbbox()
    if not box:
        continue
    glyphs[ch] = {"img": img.crop(box), "x": box[0] - pad, "y": box[1] - pad, "advance": font.getlength(ch)}

# Pack, tallest first, into rows on as many pages as needed.
pages, page, x, y, row = [], None, 0, 0, 0
order = sorted(glyphs, key=lambda c: -glyphs[c]["img"].height)
for ch in order:
    g = glyphs[ch]
    w, h = g["img"].size
    if page is None or x + w > PAGE:
        x, y, row = 0, y + row + 1, 0
    if page is None or y + h > PAGE:
        page = Image.new("RGBA", (PAGE, PAGE), OUTLINE[:3] + (0,))
        pages.append(page)
        x, y, row = 0, 0, 0
    page.paste(g["img"], (x, y))
    g.update(page=len(pages), px=x, py=y, w=w, h=h)
    x += w + 1
    row = max(row, h)

os.makedirs(out, exist_ok=True)
for i, p in enumerate(pages, 1):
    p.save(os.path.join(out, f"font_{i}.png"))
with open(os.path.join(out, "font.lua"), "w", encoding="utf-8") as f:
    f.write("-- Made by tools/make_bitmap_font.py from %s at %d px. Do not edit by hand.\n" % (os.path.basename(src), SIZE))
    f.write("-- Per character code: page, x, y, w, h in the page; ox, oy from the line's top-left; advance.\n")
    f.write("return { line = %d, space = %.2f, pages = %d, glyphs = {\n" % (line, font.getlength(" "), len(pages)))
    for ch in sorted(glyphs):
        g = glyphs[ch]
        f.write("  [%d] = { %d, %d, %d, %d, %d, %d, %d, %.2f },\n" % (ord(ch), g["page"], g["px"], g["py"], g["w"], g["h"], g["x"], g["y"], g["advance"]))
    f.write("} }\n")
print(f"FONT {len(glyphs)} glyphs on {len(pages)} pages, line {line} px, outline {STROKE} px")
