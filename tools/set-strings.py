"""Write translation keys into the eleven data/Interface/Translations/NjordlingerCombatGameplayOverhaul_<lang>.txt files
(UTF-16 LE with BOM, CRLF, "$KEY<TAB>text"). A key already present is replaced in place; a new key is appended.

usage: py -3 tools/set-strings.py <strings.json>
strings.json: { "NJ_Key": { "english": "...", "french": "...", ... }, ... } - a language missing for a key gets the English.
"""
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANGS = ["english", "french", "german", "italian", "spanish", "polish", "russian", "czech", "japanese", "chinese", "korean"]


def main(path):
    table = json.load(open(path, encoding="utf-8"))
    for lang in LANGS:
        f = os.path.join(REPO, "data", "Interface", "Translations", f"NjordlingerCombatGameplayOverhaul_{lang}.txt")
        text = open(f, encoding="utf-16").read()
        lines = text.replace("\r\n", "\n").split("\n")
        while lines and lines[-1] == "":
            lines.pop()
        index = {ln.split("\t", 1)[0]: i for i, ln in enumerate(lines) if "\t" in ln}
        changed = added = 0
        for key, tr in table.items():
            value = tr.get(lang) or tr["english"]
            assert "\t" not in value and "\n" not in value, (key, lang)
            line = f"${key}\t{value}"
            if f"${key}" in index:
                if lines[index[f"${key}"]] != line:
                    lines[index[f"${key}"]] = line
                    changed += 1
            else:
                lines.append(line)
                added += 1
        with open(f, "w", encoding="utf-16", newline="\r\n") as out:   # utf-16 writes the LE BOM
            out.write("\n".join(lines) + "\n")
        print(f"{lang}: {added} added, {changed} changed")


if __name__ == "__main__":
    main(sys.argv[1])
