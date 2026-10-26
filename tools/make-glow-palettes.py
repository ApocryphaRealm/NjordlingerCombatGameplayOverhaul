r"""Write the soul-stone glow palettes (PLAN.md phase 4, D17: a drawn weapon glows in its best stone's tier colour).

  py -3 -I make-glow-palettes.py <config/stones.json> <data folder>

Combined run ed4e: the glow read pale lavender (saturation ~0.18) - the copied enchant shader's grey flame texture lit the
colour keys to near white. A membrane PALETTE maps the texture's brightness to colours instead, so each tier gets a
256 x 1 gradient from clear through its colour to a lighter shade of it; the shader (tools/PluginBuild) points at
textures\effects\NjordlingerCGO\StoneGlow_<tier>.dds. Our own art (uncompressed RGBA DDS). Exit 0 written.
"""
import json, os, sys

from PIL import Image


def main(argv):
    cfg = json.load(open(argv[0], encoding="utf-8"))
    out = os.path.join(argv[1], "textures", "effects", "NjordlingerCGO")
    os.makedirs(out, exist_ok=True)
    for name, hexcol in zip(cfg["tiers"], cfg["tierColour"]):
        r, g, b = (int(hexcol[i:i + 2], 16) for i in (1, 3, 5))
        im = Image.new("RGBA", (256, 1))
        for x in range(256):
            t = x / 255.0
            if t < 0.6:   # clear -> the full colour
                k = t / 0.6
                px = (int(r * k), int(g * k), int(b * k), int(255 * k))
            else:         # the colour -> a lighter shade of it (still saturated: a quarter of the way to white)
                k = (t - 0.6) / 0.4 * 0.25
                px = (int(r + (255 - r) * k), int(g + (255 - g) * k), int(b + (255 - b) * k), 255)
            im.putpixel((x, 0), px)
        im.save(os.path.join(out, "StoneGlow_%s.dds" % name))
    print("wrote %d palettes into %s" % (len(cfg["tiers"]), out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
