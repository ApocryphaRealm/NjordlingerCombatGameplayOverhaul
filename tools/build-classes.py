r"""Write the phase 2 data files of Njordlinger Combat Gameplay Overhaul from the repo's config - ONE command.

  py -3 tools\build-classes.py [--forms generated\plugin-forms.json]

1. data/SKSE/Plugins/NjordlingerCombatGameplayOverhaul/classes.json - config/classes.json, with each class's form IDs
   (ability, global, keyword) and the group globals filled in from the plugin build's form list when it exists
   (generated/plugin-forms.json, written by the plugin step); without it the classes carry no forms and the DLL makes
   the group globals at run time.
2. data/SKSE/Plugins/NjordlingerCombatGameplayOverhaul/quest-rewards.json - config/quest-rewards.json (the rows the data
   build generates from the main-quest and Miraak records).
3. data/SKSE/Plugins/NjordlingerCombatGameplayOverhaul/gear.json - the Gear module's forms (phase 3: set keywords and
   abilities, unique keywords, perks and abilities, the two switch globals) from the plugin build's form list.
4. data/SKSE/Plugins/NjordlingerCombatGameplayOverhaul.ini - the shipped INI, written from the same defaults the DLL
   compiles in (source/Settings.cpp SetDefaults; rule 16). The values are listed once below; check_defaults() reads
   Settings.cpp and refuses to write when the two disagree.
No machine path is written into any file (rule 4).
"""
import argparse, json, os, re, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, "data", "SKSE", "Plugins")
SKILLS = ["OneHanded", "TwoHanded", "Archery", "Block", "Smithing", "HeavyArmor", "LightArmor", "Pickpocket", "Lockpicking",
          "Sneak", "Alchemy", "Speech", "Alteration", "Conjuration", "Destruction", "Illusion", "Restoration", "Enchanting"]

# (section, key, value, the C++ name in SetDefaults, comment)
DEFAULTS = [
    ("Debug", "uLogLevel", "2", "debug::logLevel", "0 trace, 1 debug, 2 info, 3 warning, 4 error, 5 critical, 6 off"),
    ("Skills", "bOverrideSkillCaps", "0", "skills::overrideCaps", "1 = each skill stops at fCap<Skill>; 0 = 100 as in vanilla. A class's locked skills are capped either way."),
    ("SkillExperience", "bOverrideSkillExp", "0", "skillexp::overrideRates", "1 = each use pays vanilla x fMult<Skill>, an increase pays vanilla x fSkillToLevelMult toward the level"),
    ("SkillExperience", "fSkillToLevelMult", "1.00", "skillexp::toLevelMult", ""),
    ("LevelUp", "bOverrideLevelUpRewards", "1", "levelup::overrideRewards", "1 = perk points from sPerksByLevel, attribute gains from the three values below (all 0: the attribute pick is skipped)"),
    ("LevelUp", "sPerksByLevel", "1:1", None, "level:perks pairs - from each listed level on, that many perk points per level up"),
    ("LevelUp", "fHealthPerLevel", "0.00", "levelup::healthPerLevel", ""),
    ("LevelUp", "fMagickaPerLevel", "0.00", "levelup::magickaPerLevel", ""),
    ("LevelUp", "fStaminaPerLevel", "0.00", "levelup::staminaPerLevel", ""),
    ("Classes", "bEnableClasses", "1", "classes::enabled", ""),
    ("Classes", "iClassLevelsPerTier", "10", "classes::levelsPerTier", "class levels between two evolutions"),
    ("Classes", "fClassXpPerSkillIncrease", "1.00", "classes::xpPerIncrease", ""),
    ("Classes", "fClassXpBase", "2.00", "classes::xpBase", "class level n -> n+1 costs fClassXpBase + fClassXpPerLevel x n"),
    ("Classes", "fClassXpPerLevel", "0.20", "classes::xpPerLevel", ""),
    ("Classes", "bLockPerks", "1", "classes::lockPerks", ""),
    ("Classes", "bLockSkills", "1", "classes::lockSkills", ""),
    ("Classes", "bCraftingGate", "1", "classes::craftingGate", ""),
    ("Classes", "bRaceMenuClass", "1", "classes::raceMenuClass", "the Class category in the race menu"),
    ("QuestRewards", "bEnableQuestRewards", "1", "questrewards::enabled", "rows: quest-rewards.json; a row you change is kept under [QuestRewardRows] as <EditorID>=<points>"),
    ("Gear", "bSetBonuses", "1", "gear::sets", "unique armour sets: a bonus at two worn pieces and another at four"),
    ("Gear", "bUniqueEffects", "1", "gear::uniques", "named unique items carry an effect of their own"),
    ("Gear", "fSetStrength", "1.00", "gear::setStrength", "0.25 to 2.00 - scales every set bonus from the value in the plugin (1.00 = as written)"),
    ("Gear", "fUniqueStrength", "1.00", "gear::uniqueStrength", "0.25 to 2.00 - scales every unique effect"),
    ("SoulStones", "bEnableSoulStones", "1", "stones::enabled", "soul stone effects, phrases and the Shouts phrases grant"),
    ("SoulStones", "bNameSlots", "1", "stones::nameSlots", "a socketed item's name ends with its words, e.g. Elven Sword [Yol Fo -]"),
    ("SoulStones", "bNameDragonLetters", "0", "stones::nameDragonLetters", "1 = write those words in dragon letters (needs an inventory that renders font tags)"),
    ("SoulStones", "bWeaponGlow", "1", "stones::weaponGlow", "a drawn weapon glows in the colour of its best stone's tier"),
]


def check_defaults():
    src = open(os.path.join(REPO, "source", "Settings.cpp"), encoding="utf-8").read()
    body = src[src.index("void SetDefaults()"):]
    body = body[:body.index("\n\t\t}\n")]
    bad = []
    for section, key, value, name, _ in DEFAULTS:
        if not name:
            continue
        m = re.search(re.escape(name) + r"\s*=\s*([^;]+);", body)
        if not m:
            bad.append(f"{name}: not set in SetDefaults")
            continue
        code = m.group(1).strip().rstrip("F").replace("true", "1").replace("false", "0")
        if abs(float(code) - float(value)) > 1e-6:
            bad.append(f"{name}: compiled {code}, INI {value}")
    if "levelup::perksByLevel = { { 1, 1 } }" not in body:
        bad.append("levelup::perksByLevel: compiled table is not 1:1")
    for needle in ("skills::cap[i] = kVanillaSkillCap", "skillexp::mult[i] = 1.0F"):
        if needle not in body:
            bad.append(f"per-skill default changed: {needle} not found")
    return bad


def write_ini():
    lines = ["; Njordlinger Combat Gameplay Overhaul - settings. Every value here is also the compiled default (rule 16);",
             "; the settings page in the Apocrypha Menu Framework edits this file.", ""]
    section = None
    for sec, key, value, _, comment in DEFAULTS:
        if sec != section:
            if section is not None:
                if section == "Skills":
                    lines += [f"fCap{s}=100.00" for s in SKILLS]
                if section == "SkillExperience":
                    lines += [f"fMult{s}=1.00" for s in SKILLS]
                lines.append("")
            lines.append(f"[{sec}]")
            section = sec
        if comment:
            lines.append(f"; {comment}")
        lines.append(f"{key}={value}")
    lines += ["", "[QuestRewardRows]", ""]
    path = os.path.join(OUT, "NjordlingerCombatGameplayOverhaul.ini")
    with open(path, "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(lines))
    return path


def write_classes(forms_path):
    cfg = json.load(open(os.path.join(REPO, "config", "classes.json"), encoding="utf-8"))
    content = json.load(open(os.path.join(REPO, "config", "class-content.json"), encoding="utf-8"))
    # the rank perks' names and descriptions travel with the class (the page shows them; NJ_RankName_/NJ_RankDesc_ keys
    # translate them)
    for c in cfg["classes"]:
        ranks = content.get("ranks", {}).get(c["id"])
        if ranks:
            c["ranks"] = {slot: {"name": r["name"], "description": r["description"]} for slot, r in ranks.items()}
    forms = {}
    if forms_path and os.path.exists(forms_path):
        forms = json.load(open(forms_path, encoding="utf-8"))
        for c in cfg["classes"]:
            entry = forms.get("classes", {}).get(c["id"], {})
            for kind in ("ability", "global", "keyword"):
                if entry.get(kind):
                    c[kind] = entry[kind]
            for slot, fid in entry.get("ranks", {}).items():
                c.setdefault("ranks", {}).setdefault(slot, {})["perk"] = fid
        for t in cfg.get("titles", []):
            fid = forms.get("titles", {}).get(t["id"])
            if fid:
                t["perk"] = fid
        if forms.get("groupGlobals"):
            cfg["groupGlobals"] = forms["groupGlobals"]
        if forms.get("helpers"):
            cfg["helpers"] = forms["helpers"]   # globals and spells source/ClassEffects.cpp sets and casts
    folder = os.path.join(OUT, "NjordlingerCombatGameplayOverhaul")
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, "classes.json")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(cfg, f, indent=2, ensure_ascii=False)
        f.write("\n")
    return path, bool(forms)


def write_gear(forms_path):
    if not forms_path or not os.path.exists(forms_path):
        return None, 0, 0
    gear = json.load(open(forms_path, encoding="utf-8")).get("gear")
    if not gear:
        return None, 0, 0
    path = os.path.join(OUT, "NjordlingerCombatGameplayOverhaul", "gear.json")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(gear, f, indent=2, ensure_ascii=False)
        f.write("\n")
    return path, len(gear.get("sets", [])), len(gear.get("uniques", []))


def write_quest_rows():
    src = os.path.join(REPO, "config", "quest-rewards.json")
    data = json.load(open(src, encoding="utf-8"))
    path = os.path.join(OUT, "NjordlingerCombatGameplayOverhaul", "quest-rewards.json")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
        f.write("\n")
    return path, len(data.get("rows", []))


def main(argv):
    p = argparse.ArgumentParser()
    p.add_argument("--forms", default=os.path.join(REPO, "generated", "plugin-forms.json"))
    a = p.parse_args(argv)
    bad = check_defaults()
    if bad:
        print("refused: the INI defaults and source/Settings.cpp disagree:")
        for b in bad:
            print("  " + b)
        return 1
    print("wrote", os.path.relpath(write_ini(), REPO))
    path, filled = write_classes(a.forms)
    print("wrote", os.path.relpath(path, REPO), "(form IDs filled from the plugin build)" if filled else "(no plugin form list - classes carry no forms)")
    path, ns, nu = write_gear(a.forms)
    if path:
        print("wrote", os.path.relpath(path, REPO), f"({ns} sets, {nu} uniques)")
    else:
        print("no gear forms in the plugin form list - gear.json not written")
        return 1
    path, n = write_quest_rows()
    print("wrote", os.path.relpath(path, REPO), f"({n} rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
