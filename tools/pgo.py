#!/usr/bin/env python3
"""PGO helper for the Android build (RENUT_PGO in android/native/CMakeLists.txt).

  pull    copy files/renut-*.profraw out of the instrumented app into out/pgo/
  merge   llvm-profdata merge out/pgo/*.profraw -> out/pgo/renut.profdata

Flow: gradle -PrenutPgo=gen, install, play (menu, track, Showdown, cart), leave the
game from the sidebar (nativeStop writes the profile), `pgo.py pull`, `pgo.py merge`,
then gradle -PrenutPgo=use -PrenutPgoProfile=out/pgo/renut.profdata.
"""
import glob, os, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "out" / "pgo"
SDK = Path(os.environ.get("ANDROID_HOME") or r"C:\Users\Sidney-PC\AppData\Local\Android\Sdk")
ADB = os.environ.get("ADB") or str(SDK / "platform-tools" / "adb.exe")
NDK = SDK / "ndk" / "29.0.14206865"
PKG = "org.renut.android"


def pull():
    OUT.mkdir(parents=True, exist_ok=True)
    names = subprocess.run([ADB, "shell", f"run-as {PKG} ls files"], capture_output=True, text=True).stdout.split()
    raws = [n for n in names if n.startswith("renut-") and n.endswith(".profraw")]
    if not raws:
        sys.exit("no renut-*.profraw in the app's files (was it the instrumented build, and did the game stop?)")
    for n in raws:
        with open(OUT / n, "wb") as f:
            subprocess.run([ADB, "exec-out", f"run-as {PKG} cat files/{n}"], stdout=f, check=True)
        print("pulled", n, (OUT / n).stat().st_size)


def merge():
    tool = next(iter(glob.glob(str(NDK / "toolchains" / "llvm" / "prebuilt" / "*" / "bin" / "llvm-profdata*"))), None)
    if not tool:
        sys.exit("llvm-profdata not found in the NDK")
    raws = sorted(glob.glob(str(OUT / "*.profraw")))
    if not raws:
        sys.exit("nothing to merge in out/pgo")
    subprocess.run([tool, "merge", "-output", str(OUT / "renut.profdata")] + raws, check=True)
    print("wrote", OUT / "renut.profdata")


if __name__ == "__main__":
    {"pull": pull, "merge": merge}.get(sys.argv[1] if len(sys.argv) > 1 else "", lambda: sys.exit(__doc__))()
