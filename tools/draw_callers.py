#!/usr/bin/env python3
"""Names the draw callers of the log (cvar renut_plume_draw_callers) with the unstripped runtime library.

  python tools/draw_callers.py rexglue-runtime.log librenut_runtime.so [--depth 3] [--top 40]
"""
import argparse
import collections
import glob
import os
import re
import subprocess
import sys

LINE = re.compile(
    r"Plume caller ([0-9A-F]{8}): draws (\d+) depth-only (\d+) indices (\d+) hiz tested (\d+) no-model (\d+) "
    r"dropped (\d+) \((\d+) indices\) frames((?: [0-9A-F]+)+)")


def find_tool():
    ndk = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Android", "Sdk", "ndk")
    found = sorted(glob.glob(os.path.join(ndk, "*", "toolchains", "llvm", "prebuilt", "*", "bin", "llvm-addr2line*")))
    return found[-1] if found else "llvm-addr2line"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("library")
    ap.add_argument("--depth", type=int, default=3, help="game functions that name a group")
    ap.add_argument("--top", type=int, default=40)
    args = ap.parse_args()

    rows = []
    for text in open(args.log, encoding="utf-8", errors="replace"):
        m = LINE.search(text)
        if m:
            vals = [int(x) for x in m.groups()[1:8]]
            frames = [int(x, 16) for x in m.group(9).split()]
            rows.append((m.group(1), vals, frames))
    if not rows:
        sys.exit("no caller lines in the log (renut_plume_draw_callers on?)")

    wanted = sorted({f - 4 for _, _, frames in rows for f in frames if f})
    out = subprocess.run([find_tool(), "-f", "-C", "-e", args.library], input="\n".join(hex(a) for a in wanted),
                         capture_output=True, text=True).stdout.splitlines()
    names = {}
    for i, a in enumerate(wanted):
        names[a + 4] = out[2 * i] if 2 * i < len(out) else "?"

    def short(f):
        n = names.get(f, "?") if f else "-"
        m = re.search(r"sub_[0-9A-Fa-f]{8}", n)
        return m.group(0) if m else n.split("(")[0].split("::")[-1]

    groups = collections.OrderedDict()
    for _, vals, frames in rows:
        keys = [short(f) for f in frames if f]
        game = [k for k in keys if k.startswith("sub_")][: args.depth]
        key = " <- ".join(game) if game else " <- ".join(keys[:3])
        g = groups.setdefault(key, [0] * 7)
        for i, v in enumerate(vals):
            g[i] += v

    print("%9s %9s %9s %9s %9s %9s %11s  %s" % ("draws", "depth", "tested", "nomodel", "dropped", "drop%", "indices", "callers"))
    for key, g in sorted(groups.items(), key=lambda kv: -kv[1][0])[: args.top]:
        draws, depth, indices, tested, nomodel, dropped, dropped_idx = g
        pct = 100.0 * dropped / draws if draws else 0.0
        print("%9d %9d %9d %9d %9d %8.1f%% %11d  %s" % (draws, depth, tested, nomodel, dropped, pct, indices, key))


if __name__ == "__main__":
    main()
