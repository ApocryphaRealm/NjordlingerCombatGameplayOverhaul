r"""Build Njordlinger Combat Gameplay Overhaul's generated data files against an MO2 profile - ONE command (phase 1).

  py -3 build-data.py --instance <MO2 instance folder> --stager <stage script> [--profile "Njordlinger Test"]
                      [--profile-dir <folder>] [--work <scratch folder>] [--keep-stage]

--instance defaults to the environment variable NJCGO_MO2_INSTANCE and --stager to NJCGO_STAGER: a script that stages
a profile's active plugins into <out>/Data plus <out>/plugins.txt (the Njordlinger tools' stage_synthesis_data.py,
arguments --instance --profile --out [--profile-dir]). --work defaults to _work/njcgo two folders above the repo. No
machine path is written into this file (rule 4).

Re-run it after the mod list changes (a weapon or armour mod added, a perk overhaul swapped, an NPC mod added): the tiers
and NPC levels follow the load order as it is then. Nothing is written into the instance.

  1. stages the profile's active plugins into a fresh scratch folder (hardlinks). With --profile-dir the three profile
     files are read from that folder instead (a SIMULATED profile, e.g. a mod unticked);
  2. builds tools/TierBuild (dotnet) and runs it: tiers, set keywords, NPC levels by lore tier;
  3. copies NjordlingerCGO_Tiers_KID.ini, NjordlingerCGO_Sets_KID.ini and SKSE/Plugins/SkyPatcher/npc/NjordlingerCGO/
     LoreTierLevels.ini into data/ (the mod folder's content) and build-report.txt into generated/ (tier-audit.tsv, the
     per-item list with the reason for each tier, stays in the scratch folder);
  4. runs the tool again in verify mode: every plugin|FormID in data/ must resolve in the staged load order;
  5. deletes the staging folder unless --keep-stage.
Exit 0 built and verified, 1 a step failed, 2 a reference does not resolve.
"""
import argparse, os, shutil, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECT = os.path.join(REPO, "tools", "TierBuild")
DATA = os.path.join(REPO, "data")
GENERATED = os.path.join(REPO, "generated")
COPIES = ["NjordlingerCGO_Tiers_KID.ini", "NjordlingerCGO_Sets_KID.ini",
          os.path.join("SKSE", "Plugins", "SkyPatcher", "npc", "NjordlingerCGO", "LoreTierLevels.ini")]


def run(cmd, env=None, **kw):
    print(">", " ".join('"%s"' % c if " " in c else c for c in cmd))
    return subprocess.run(cmd, env=env, **kw)


def tool(exe, stage, out, extra_env):
    env = dict(os.environ, NJCGO_CONFIG=os.path.join(REPO, "config"), NJCGO_OUT=out, **extra_env)
    return run([exe, "run-patcher", "--GameRelease", "SkyrimSE", "--DataFolderPath", os.path.join(stage, "Data"),
                "--LoadOrderFilePath", os.path.join(stage, "plugins.txt"),
                "--OutputPath", os.path.join(out, "discarded-empty-patch.esp")], env=env).returncode


def main(argv):
    p = argparse.ArgumentParser()
    p.add_argument("--instance", default=os.environ.get("NJCGO_MO2_INSTANCE"))
    p.add_argument("--stager", default=os.environ.get("NJCGO_STAGER"))
    p.add_argument("--profile", default="Njordlinger Test")
    p.add_argument("--profile-dir")
    p.add_argument("--work", default=os.path.join(os.path.dirname(os.path.dirname(REPO)), "_work", "njcgo"))
    p.add_argument("--keep-stage", action="store_true")
    a = p.parse_args(argv)
    if not a.instance or not a.stager:
        print("refused: give --instance and --stager (or set NJCGO_MO2_INSTANCE / NJCGO_STAGER)")
        return 1
    bin_dir = os.path.join(a.work, "tierbuild-bin")
    exe = os.path.join(bin_dir, "TierBuild.exe")
    stamp = time.strftime("%Y%m%d-%H%M%S")
    stage = os.path.join(a.work, "stage-" + stamp)
    out = os.path.join(a.work, "gen-" + stamp)
    try:
        if run(["dotnet", "build", "-c", "Release", "-o", bin_dir], cwd=PROJECT).returncode:
            return 1
        cmd = [sys.executable, a.stager, "--instance", a.instance, "--profile", a.profile, "--out", stage]
        if a.profile_dir:
            cmd += ["--profile-dir", a.profile_dir]
        if run(cmd).returncode:
            return 1
        if tool(exe, stage, out, {}):
            return 1
        for rel in COPIES:
            src = os.path.join(out, rel)
            if not os.path.isfile(src):
                print("missing output", src)
                return 1
            dst = os.path.join(DATA, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(src, dst)
        os.makedirs(GENERATED, exist_ok=True)
        shutil.copyfile(os.path.join(out, "build-report.txt"), os.path.join(GENERATED, "build-report.txt"))
        if tool(exe, stage, out, {"NJCGO_VERIFY": DATA}):
            return 1
        verdict = open(os.path.join(out, "verify-exit.txt")).read().strip()
        text = open(os.path.join(out, "verify-report.txt"), encoding="utf-8").read().replace(DATA, "data")
        with open(os.path.join(GENERATED, "verify-report.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("outputs:", out)
        return 0 if verdict == "0" else 2
    finally:
        if not a.keep_stage and os.path.isdir(stage):
            shutil.rmtree(stage, ignore_errors=True)   # hardlinks only: the instance's own files are untouched


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
