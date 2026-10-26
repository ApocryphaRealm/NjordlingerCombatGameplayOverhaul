r"""Render the soul-stone glyph atlas from the game's own dragon font (PLAN.md phase 4 step 1, D17; FEASIBILITY 12.2).

  py -3 -I make-glyphs.py <Dragon_script.ttf> <stone-scheme.json> <out folder>

The font: the vanilla UI font Dragon_script inside Interface\fonts_en.swf (Skyrim - Interface.bsa), mapped to $DragonFont
by fontconfig.txt. Get it with BSArch (unpack the archive) and FFDec (`ffdec-cli -format font:ttf -export font <out>
fonts_en.swf`); nothing of a third party is used. Writes, per word, glyph_<Word>.png (128 x 128, white on transparent,
the word's letters side by side), and atlas.png (8 x 8 cells of 128, words in stone-scheme order, cell = index) with
atlas.json (word -> cell and UV rectangle). Exit 0 written, 1 refused.

The font draws the 25 plain letters as A-Z and the dragon alphabet's nine doubled letters as the digits 1-9. DIGRAPHS
below is the order assumed for them (aa, ei, ii, ir, oo, uu, ah, ey, ur - the doubled letters in the alphabet's order);
it is UNVERIFIED against the word walls - check one rendered word with a doubled letter against a wall before shipping
the atlas anywhere a player sees it.
"""
import json, os, sys

from PIL import Image, ImageDraw, ImageFont

DIGRAPHS = {"AA": "1", "EI": "2", "II": "3", "IR": "4", "OO": "5", "UU": "6", "AH": "7", "EY": "8", "UR": "9"}
CELL = 128


def encode(word):
    w, out, i = word.upper(), [], 0
    while i < len(w):
        pair = w[i:i + 2]
        if pair in DIGRAPHS:
            out.append(DIGRAPHS[pair])
            i += 2
        else:
            out.append(w[i])
            i += 1
    return "".join(out)


def render(font_path, text):
    size = CELL
    while size > 8:
        font = ImageFont.truetype(font_path, size)
        box = font.getbbox(text)
        if box[2] - box[0] <= CELL - 12 and box[3] - box[1] <= CELL - 12:
            break
        size -= 4
    img = Image.new("RGBA", (CELL, CELL), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    box = font.getbbox(text)
    x = (CELL - (box[2] - box[0])) // 2 - box[0]
    y = (CELL - (box[3] - box[1])) // 2 - box[1]
    d.text((x, y), text, font=font, fill=(255, 255, 255, 255))
    return img


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 1
    ttf, scheme_path, out = argv
    words = [w["word"] for w in json.load(open(scheme_path, encoding="utf-8"))["words"]]
    if len(words) != 60:
        print("refused: the stone scheme does not hold 60 words")
        return 1
    os.makedirs(out, exist_ok=True)
    atlas = Image.new("RGBA", (CELL * 8, CELL * 8), (0, 0, 0, 0))
    index = {}
    for i, word in enumerate(words):
        img = render(ttf, encode(word))
        img.save(os.path.join(out, "glyph_%s.png" % word))
        cx, cy = (i % 8) * CELL, (i // 8) * CELL
        atlas.paste(img, (cx, cy))
        index[word] = {"cell": i, "text": encode(word), "uv": [cx / atlas.width, cy / atlas.height, (cx + CELL) / atlas.width, (cy + CELL) / atlas.height]}
    atlas.save(os.path.join(out, "atlas.png"))
    with open(os.path.join(out, "atlas.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump({"cell": CELL, "digraphs": DIGRAPHS, "verified": False, "words": index}, f, indent=2)
    print("wrote %d glyphs and atlas.png (%dx%d) into %s" % (len(words), atlas.width, atlas.height, out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
