#!/usr/bin/env python3
"""Short frame-cost A/B of a live switch: GPU Mcycles per frame, hands off.

Flips a control file under files/ between states in an A B A B order, waits for
the new pipelines, samples the game layer's frame counter, the GPU clock and
its busy share once a second, and prints per window and per state

    Mcycles per frame = busy x clock / fps

which does not move with the clock the governor picks (a frame is clock-bound),
unlike the frame time. Needs the game in the foreground on a steady scene.

    python tools/quick-fps.py                       # plume-relconst 0,2 x 2
    python tools/quick-fps.py --file plume-fetchspec --states 0,1
"""
import argparse, os, re, subprocess, sys, time

ADB = os.environ.get("ADB") or r"C:\Users\Sidney-PC\AppData\Local\Android\Sdk\platform-tools\adb.exe"
PKG = "org.renut.android"
K = "/sys/class/kgsl/kgsl-3d0"


def adb(*args):
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    return subprocess.run([ADB, *args], capture_output=True, timeout=60, env=env).stdout.decode("utf-8", "replace")


def sample():
    out = adb("shell", f"dumpsys SurfaceFlinger | grep -A1 'Layer \\[.*SurfaceView\\[org.renut.*BLAST' | head -2; "
                       f"cat {K}/clock_mhz {K}/gpu_busy_percentage")
    m = re.search(r"frame=(\d+)", out)
    nums = re.findall(r"^\s*(\d+)\s*%?\s*$", out, re.M)
    if not m or len(nums) < 2:
        return None
    return time.time(), int(m[1]), float(nums[-2]), float(nums[-1])


def window(seconds):
    rows = []
    end = time.time() + seconds
    while time.time() < end:
        s = sample()
        if s:
            rows.append(s)
        time.sleep(0.4)
    if len(rows) < 5:
        return None
    t0, f0 = rows[0][:2]
    t1, f1 = rows[-1][:2]
    fps = (f1 - f0) / (t1 - t0)
    clk = sum(r[2] for r in rows) / len(rows)
    busy = sum(r[3] for r in rows) / len(rows) / 100
    return fps, clk, busy, busy * clk / fps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", default="plume-relconst")
    ap.add_argument("--states", default="0,2")
    ap.add_argument("--rounds", type=int, default=2)
    ap.add_argument("--seconds", type=int, default=20)
    ap.add_argument("--settle", type=int, default=8)
    args = ap.parse_args()
    if PKG not in adb("shell", "dumpsys window | grep mCurrentFocus"):
        sys.exit("the game is not in the foreground")
    original = adb("shell", f"run-as {PKG} sh -c 'cat files/{args.file} 2>/dev/null'").strip()
    states = args.states.split(",")
    results = {}
    try:
        for r in range(args.rounds):
            for st in states if r % 2 == 0 else states[::-1]:
                adb("shell", f"run-as {PKG} sh -c 'echo {st} > files/{args.file}'")
                time.sleep(args.settle)
                w = window(args.seconds)
                if not w:
                    print(f"state {st} round {r}: no samples")
                    continue
                results.setdefault(st, []).append(w)
                print(f"state {st} round {r}: fps {w[0]:5.2f}  clock {w[1]:4.0f} MHz  busy {100 * w[2]:3.0f}%  "
                      f"{w[3]:5.2f} Mcycles/frame")
    finally:
        adb("shell", f"run-as {PKG} sh -c 'echo {original or 0} > files/{args.file}'")
    print()
    base = None
    for st in states:
        ws = results.get(st, [])
        if not ws:
            continue
        mc = sum(w[3] for w in ws) / len(ws)
        fps = sum(w[0] for w in ws) / len(ws)
        spread = max(w[3] for w in ws) - min(w[3] for w in ws)
        base = base or mc
        print(f"state {st}: {mc:5.2f} Mcycles/frame (windows differ by {spread:.2f}), {fps:5.2f} fps, "
              f"{100 * (mc / base - 1):+.1f}% vs state {states[0]}")


if __name__ == "__main__":
    main()
