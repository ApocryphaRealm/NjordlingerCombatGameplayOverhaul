r"""Write the word table's 'First numbers' section from config/stones.json (PLAN.md phase 4: WORD-TABLE.md with first numbers).

  py -3 -I word-numbers.py <config/stones.json> <WORD-TABLE.md>

Replaces the block between '<!-- FIRST-NUMBERS:START -->' and '<!-- FIRST-NUMBERS:END -->' (appends it the first time), so
the numbers in the planning document are always the ones the build uses. Exit 0 written, 1 refused.
"""
import json, sys

START, END = "<!-- FIRST-NUMBERS:START -->", "<!-- FIRST-NUMBERS:END -->"
TIERS = 5


def band(v):
    if v is None:
        return ""
    if isinstance(v, list):
        lo, hi = v[0], v[-1]
        vals = [lo + (hi - lo) * t / (TIERS - 1) for t in range(TIERS)]
        return "/".join(("%g" % round(x, 3)) for x in vals)
    return "%g" % v


def cell_text(c):
    k = c["k"]
    parts = [k]
    if k == "rec":
        what = c.get("a") or c.get("av", "")
        parts.append(what + ("" if not c.get("det") else " -"))
        parts.append(band(c.get("m")))
        if c.get("dur") is not None:
            parts.append("for " + band(c["dur"]) + " s")
        if c.get("chance"):
            parts.append("%d%% chance" % c["chance"])
    elif k == "perk":
        parts.append(c["ep"] + " x" + band(c.get("mult") or c.get("add")))
    elif k == "self":
        parts.append(c["av"] + " " + band(c.get("m")))
    elif k == "proc":
        p = c["proc"]
        trig = p.get("on", "")
        flags = [f for f in ("power", "sneak", "blocked", "lowHealth", "storm") if p.get(f)]
        cast = p.get("cast", {})
        what = cast.get("shout", "") and "%s %s" % (cast["shout"], cast.get("level", 1)) or cast.get("helper") or cast.get("edid") or ("damage %g" % p["damage"] if "damage" in p else "")
        parts.append(trig + ("(" + ",".join(flags) + ")" if flags else ""))
        if p.get("chance"):
            parts.append("%d%%" % p["chance"])
        if what:
            parts.append(what)
        if p.get("mag") is not None:
            parts.append("mag " + band(p["mag"]))
        if p.get("mult") is not None:
            parts.append("x" + band(p["mult"]))
        if p.get("cd"):
            parts.append("cd %g s" % p["cd"])
    elif k == "grant":
        parts.append(c["grant"])
    if c.get("approx"):
        parts.append("(approx)")
    return " ".join(x for x in parts if x).replace("|", "/")


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 1
    cfg = json.load(open(argv[0], encoding="utf-8"))
    out = [START, "", "## First numbers (TUNE defaults, 2026-10-26 - for Fable's review)", "",
           "Generated from `config/stones.json` by `tools/word-numbers.py`; edit the JSON, not this block. Rules:",
           "- A band `lo-hi` above becomes five numbers petty/lesser/common/greater/grand, linear between the ends.",
           "- A written CHANCE band (\"10-30%\") becomes a fixed chance (the middle, or a round figure) and the tier scales the",
           "  effect's strength or duration instead - one magic effect per word and host carries the chance as its condition.",
           "- Phrases do not scale with tier. Proc cooldowns 60 s and 90 s as written; low health is below %d%%." % round(cfg["lowHealth"] * 100),
           "- Kinds: rec = the item's composed enchantment does it; perk = a perk the SKSE plugin gives while the item is",
           "  equipped (value by tier, the highest tier worn wins); self = an ability likewise; proc = the plugin's hit / hit-by /",
           "  kill handler; grant = the Shout clone (D21); stub = not built in this build (the card still shows the line).",
           "- Stones: tier values %s gold; loot: one boss chest roll in %d%% carries a stone, tier by level from %s." % (
               "/".join(str(v) for v in cfg["tierValue"]), 100 - cfg["lootChanceNone"], "/".join(str(v) for v in cfg["lootLevels"])),
           ""]
    for ph in cfg["phrases"]:
        out.append("**%s - %s**" % (" ".join(ph["words"]), ph["shout"]))
        out.append("")
        out.append("| Word | Melee | Ranged | Armour |")
        out.append("|---|---|---|---|")
        for w in ph["words"]:
            c = ph["word"][w]
            out.append("| %s | %s | %s | %s |" % (w, cell_text(c["melee"]), cell_text(c["ranged"]), cell_text(c["armour"])))
        p = ph["phrase"]
        out.append("| phrase | %s | %s | %s |" % (cell_text(p["melee"]), cell_text(p["ranged"]), cell_text(p["armour"])))
        out.append("")
    out.append(END)
    block = "\n".join(out)
    doc = open(argv[1], encoding="utf-8").read()
    if START in doc and END in doc:
        doc = doc[:doc.index(START)] + block + doc[doc.index(END) + len(END):]
    else:
        doc = doc.rstrip("\n") + "\n\n" + block + "\n"
    with open(argv[1], "w", encoding="utf-8", newline="\n") as f:
        f.write(doc)
    print("wrote the first numbers (%d phrases) into %s" % (len(cfg["phrases"]), argv[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
