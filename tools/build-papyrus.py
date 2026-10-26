r"""Compile the runesmith's Papyrus fragments (PLAN.md phase 4 step 3) into data/Scripts/*.pex - ONE command.

  py -3 -I build-papyrus.py [--compiler <folder holding PapyrusCompiler.exe>] [--headers <folder holding TopicInfo.psc>]

--compiler defaults to the environment variable NJCGO_PAPYRUS_COMPILER and --headers to NJCGO_PAPYRUS_HEADERS (the game's
own script sources, Scripts\Source - they hold TESV_Papyrus_Flags.flg). No machine path is written into this file
(rule 4). The fragments only set the quest's stages and open the barter menu; the socketing logic is in the SKSE plugin.
The .psc sources ship beside the .pex (data/Scripts/Source/) because the mod is GPL-3.0-or-later.
Exit 0 compiled, 1 refused or failed.
"""
import argparse, glob, os, shutil, subprocess, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main(argv):
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", default=os.environ.get("NJCGO_PAPYRUS_COMPILER"))
    p.add_argument("--headers", default=os.environ.get("NJCGO_PAPYRUS_HEADERS"))
    a = p.parse_args(argv)
    if not a.compiler or not a.headers:
        print("refused: give --compiler and --headers (or set NJCGO_PAPYRUS_COMPILER / NJCGO_PAPYRUS_HEADERS)")
        return 1
    exe = os.path.join(a.compiler, "PapyrusCompiler.exe")
    flags = os.path.join(a.headers, "TESV_Papyrus_Flags.flg")
    if not os.path.isfile(exe) or not os.path.isfile(flags):
        print("refused: %s or %s is missing" % (exe, flags))
        return 1
    src = os.path.join(REPO, "papyrus")
    out = os.path.join(REPO, "data", "Scripts")
    os.makedirs(os.path.join(out, "Source"), exist_ok=True)
    failed = 0
    for psc in sorted(glob.glob(os.path.join(src, "*.psc"))):
        r = subprocess.run([exe, psc, "-f=" + flags, "-i=" + src + ";" + a.headers, "-o=" + out],
                           capture_output=True, text=True)
        name = os.path.splitext(os.path.basename(psc))[0]
        ok = r.returncode == 0 and os.path.isfile(os.path.join(out, name + ".pex"))
        print(("ok    " if ok else "FAIL  ") + name)
        if not ok:
            failed += 1
            print(r.stdout[-2000:], r.stderr[-2000:])
        else:
            scrub(os.path.join(out, name + ".pex"))
        shutil.copy2(psc, os.path.join(out, "Source", os.path.basename(psc)))
    return 1 if failed else 0


def scrub(pex):
    """The compiler writes the builder's user and machine names into the PEX header (rule 45: no build-machine details
    in a shipped file). Header: magic 4, version 2, game 2, time 8, then three big-endian length-prefixed strings -
    source file, user, machine; the user and machine become neutral words, the source keeps its bare file name."""
    d = open(pex, "rb").read()
    pos = 16
    strings = []
    for _ in range(3):
        n = int.from_bytes(d[pos:pos + 2], "big")
        strings.append(d[pos + 2:pos + 2 + n])
        pos += 2 + n
    src = os.path.basename(strings[0].decode("ascii", "replace")).encode("ascii")
    new = b"".join(len(s).to_bytes(2, "big") + s for s in (src, b"author", b"build"))
    open(pex, "wb").write(d[:16] + new + d[pos:])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
