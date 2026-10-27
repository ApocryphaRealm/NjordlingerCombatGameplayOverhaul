"""Write the Custom Skills Framework 3.2.0 ring file SKILLS.json for the Test profile (CUSTOM-TREES.md; the owner 01:34:
"Move the custom trees into the skills ring").

The ring: the eighteen vanilla skills in the game's own order, then Diving (Underwater NG) and Necromancer, with our
20-slot sky (data/meshes/interface/NjordlingerCGO/intperkskydome20.nif - the slot count must equal the tree count or the
menu crashes). No perkPoints: Diving and Necromancer spend normal perk points (the owner's default until he answers).
Each custom tree is written out in full from its mod's own file, on the same globals, so its level and experience stay
shared with the original. Test-only: the output goes to testonly/CustomSkillsRing, installed as its own MO2 mod.

usage: py -3 tools/make-skills-ring.py --underwater <UnderwaterNG.json> --necromancer <CustomSkill.Necromancer.config.txt>
"""
import argparse
import json
import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VANILLA = ["Enchanting", "Smithing", "HeavyArmor", "Block", "TwoHanded", "OneHanded", "Marksman", "LightArmor", "Sneak", "Lockpicking",
           "Pickpocket", "Speechcraft", "Alchemy", "Illusion", "Conjuration", "Destruction", "Restoration", "Alteration"]   # the game's own order (combined run eacb, logged with the ring unticked) - the vanilla sky's cImage00-17 follow it
SKY = "Interface/NjordlingerCGO/intperkskydome20.nif"


def ref(plugin, local):
    if not plugin or not local or int(local, 0) == 0:
        return None
    return f"{plugin}|{int(local, 0):X}"


def necromancer(path):
    text = open(path, encoding="utf-8-sig", errors="replace").read()
    kv = {}
    for line in text.splitlines():
        m = re.match(r'\s*([A-Za-z0-9_.]+)\s*=\s*(.*?)\s*$', line)
        if m and not line.lstrip().startswith("#"):
            kv[m.group(1)] = m.group(2).strip('"')

    def glob(name):
        return ref(kv.get(name + "File", ""), kv.get(name + "Id", "0"))

    nodes = []
    ids = {}
    n = 1
    while f"Node{n}.PerkId" in kv:
        ids[n] = f"node{n}"
        n += 1
    for i in range(1, n):
        if kv.get(f"Node{i}.Enable", "true").lower() != "true":
            continue
        links = [ids[int(x)] for x in kv.get(f"Node{i}.Links", "").split() if x.isdigit() and int(x) in ids]
        node = {"id": ids[i], "perk": ref(kv[f"Node{i}.PerkFile"], kv[f"Node{i}.PerkId"]),
                "x": float(kv.get(f"Node{i}.GridX", "0")) + float(kv.get(f"Node{i}.X", "0")),
                "y": float(kv.get(f"Node{i}.GridY", "0")) + float(kv.get(f"Node{i}.Y", "0"))}
        if links:
            node["links"] = links
        nodes.append(node)
    return {"id": "Necromancer", "name": kv.get("Name", "Necromancer").title(), "description": kv.get("Description", ""),
            "level": glob("Level"), "ratio": glob("Ratio"), "legendary": glob("Legendary"), "color": glob("Color"),
            "showLevelup": glob("ShowLevelup"),
            "experienceFormula": {"useMult": 1.0, "useOffset": 0.0, "improveMult": 1.0, "improveOffset": 0.0, "enableXPPerRank": False},
            "nodes": nodes}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--underwater", required=True)
    ap.add_argument("--necromancer", required=True)
    ap.add_argument("--order", help="the game's own vanilla tree order, as the 'stats menu: tree order' log line prints it with the ring unticked (e.g. '0=Enchanting 1=Smithing ...'); the vanilla sky's pictures follow it")
    a = ap.parse_args()
    global VANILLA
    if a.order:
        ini2csf = {"OneHanded": "OneHanded", "TwoHanded": "TwoHanded", "Archery": "Marksman", "Block": "Block", "Smithing": "Smithing", "HeavyArmor": "HeavyArmor",
                   "LightArmor": "LightArmor", "Pickpocket": "Pickpocket", "Lockpicking": "Lockpicking", "Sneak": "Sneak", "Alchemy": "Alchemy", "Speech": "Speechcraft",
                   "Alteration": "Alteration", "Conjuration": "Conjuration", "Destruction": "Destruction", "Illusion": "Illusion", "Restoration": "Restoration", "Enchanting": "Enchanting"}
        pairs = sorted((int(k), v) for k, v in (t.split("=", 1) for t in a.order.split()))
        VANILLA = [ini2csf[v] for _, v in pairs][:18]
        assert len(VANILLA) == 18 and len(set(VANILLA)) == 18, VANILLA
    diving = json.load(open(a.underwater, encoding="utf-8"))["skills"][0]
    ring = {"$schema": "https://raw.githubusercontent.com/Exit-9B/CustomSkills/main/docs/schema/CustomSkill.json", "version": 1,
            "skydome": {"model": SKY, "cameraRightPoint": 1}, "skills": VANILLA + [diving, necromancer(a.necromancer)]}
    out = os.path.join(REPO, "testonly", "CustomSkillsRing", "SKSE", "Plugins", "CustomSkills", "SKILLS.json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(ring, f, indent=2, ensure_ascii=False)
        f.write("\n")
    # the order the SKSE plugin reads to know which ring slot is which tree
    order = {"note": "GENERATED by tools/make-skills-ring.py - the SKILLS.json ring order (vanilla names as the framework writes them, then custom skill ids)",
             "ring": VANILLA + [diving["id"], "Necromancer"]}
    o2 = os.path.join(REPO, "testonly", "CustomSkillsRing", "SKSE", "Plugins", "NjordlingerCombatGameplayOverhaul", "skills-ring.json")
    os.makedirs(os.path.dirname(o2), exist_ok=True)
    with open(o2, "w", encoding="utf-8", newline="\n") as f:
        json.dump(order, f, indent=2)
        f.write("\n")
    print(f"{out}: {len(ring['skills'])} trees (sky {SKY}); Necromancer {len(ring['skills'][-1]['nodes'])} nodes, Diving {len(diving.get('nodes', []))} nodes")


if __name__ == "__main__":
    main()
