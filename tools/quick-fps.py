#!/usr/bin/env python3
"""Short frame-cost A/B of a live switch: GPU Mcycles per frame, hands off.

Flips a control file under files/ between states in an A B A B order, waits for
the new pipelines, samples the game layer's frame counter, the GPU clock and
its busy share once a second, and prints per window and per state

    Mcycles per frame = busy x clock / fps

which does not move with the clock the governor picks (a frame is clock-bound),
unlike the frame time. Needs the game in the foreground on a steady scene.

    python tools/quick-fps.py --cvar renut_plume_hiz --states 2,3
    python tools/quick-fps.py --cvar renut_plume_hiz --states 2,3 --restore 2

--cvar sets a cvar live through the debug build's broadcast receiver
(RuntimeActivity.registerCvarReceiver); the Plume switches are cvars read every
frame, no longer files. --file still writes files/<name> for anything that reads one.
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


THREADS = ("GPU Commands", "PlumeRecord")


def thread_ticks():
    """CPU ticks (utime + stime, 100 a second) of the renderer's threads, by name."""
    # Another app's /proc is closed to the shell; run-as reads it as the app.
    out = adb("shell", f"run-as {PKG} sh -c 'for t in /proc/$(pidof {PKG}:runtime)/task/*; do cat $t/stat; done 2>/dev/null'")
    ticks = {}
    for line in out.splitlines():
        m = re.match(r"\d+ \((.*)\) (.*)", line)
        # Names are cut to 15 characters ("GPU Commands (F").
        name = next((t for t in THREADS if m and m[1].startswith(t[:15])), None) if m else None
        if not name:
            continue
        rest = m[2].split()
        ticks[name] = ticks.get(name, 0) + int(rest[11]) + int(rest[12])
    return ticks


def window(seconds):
    rows = []
    cpu0 = thread_ticks()
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
    cpu1 = thread_ticks()
    frames = max(f1 - f0, 1)
    # ms of CPU per frame for each thread (a tick is 10 ms).
    cpu = {name: 10.0 * (cpu1.get(name, 0) - cpu0.get(name, 0)) / frames for name in THREADS}
    return fps, clk, busy, busy * clk / fps, cpu


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", default=None)
    ap.add_argument("--cvar", default=None)
    ap.add_argument("--restore", default=None, help="--cvar: value to leave set (default: the first state)")
    ap.add_argument("--states", default="0,2")
    ap.add_argument("--rounds", type=int, default=2)
    ap.add_argument("--seconds", type=int, default=20)
    ap.add_argument("--settle", type=int, default=8)
    args = ap.parse_args()
    if PKG not in adb("shell", "dumpsys window | grep mCurrentFocus"):
        sys.exit("the game is not in the foreground")
    if bool(args.file) == bool(args.cvar):
        sys.exit("give exactly one of --file and --cvar")
    states = args.states.split(",")
    if args.cvar:
        def set_state(value):
            out = adb("shell", f"am broadcast -a org.renut.android.SET_CVAR -p {PKG} --es name {args.cvar} --es value {value}")
            if "result=1" not in out:
                sys.exit(f"the game did not take {args.cvar}={value}: {out.strip()}")
        original = args.restore or states[0]
    else:
        def set_state(value):
            adb("shell", f"run-as {PKG} sh -c 'echo {value} > files/{args.file}'")
        original = adb("shell", f"run-as {PKG} sh -c 'cat files/{args.file} 2>/dev/null'").strip() or "0"
    results = {}
    try:
        for r in range(args.rounds):
            for st in states if r % 2 == 0 else states[::-1]:
                set_state(st)
                time.sleep(args.settle)
                w = window(args.seconds)
                if not w:
                    print(f"state {st} round {r}: no samples")
                    continue
                results.setdefault(st, []).append(w)
                print(f"state {st} round {r}: fps {w[0]:5.2f}  clock {w[1]:4.0f} MHz  busy {100 * w[2]:3.0f}%  "
                      f"{w[3]:5.2f} Mcycles/frame  cp {w[4]['GPU Commands']:4.1f} rec {w[4]['PlumeRecord']:4.1f} ms/frame")
    finally:
        set_state(original)
    print()
    base = None
    for st in states:
        ws = results.get(st, [])
        if not ws:
            continue
        mc = sum(w[3] for w in ws) / len(ws)
        fps = sum(w[0] for w in ws) / len(ws)
        cp = sum(w[4]["GPU Commands"] for w in ws) / len(ws)
        rec = sum(w[4]["PlumeRecord"] for w in ws) / len(ws)
        spread = max(w[3] for w in ws) - min(w[3] for w in ws)
        base = base or mc
        print(f"state {st}: {mc:5.2f} Mcycles/frame (windows differ by {spread:.2f}), {fps:5.2f} fps, "
              f"{100 * (mc / base - 1):+.1f}% vs state {states[0]}; CPU ms/frame: commands {cp:.1f}, recorder {rec:.1f}")


if __name__ == "__main__":
    main()
