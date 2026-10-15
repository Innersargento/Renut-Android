#!/usr/bin/env python3
"""A/B of a live Plume switch on the command thread's CPU time, hands off.

Like quick-fps.py, but the number is what the command thread ("GPU Commands")
spent on the CPU per presented frame, read from its schedstat before and after
each window, next to the frame rate and the GPU clock and busy share. Use it
for a change that is a CPU cost (a cache, a hash), where Mcycles/frame of the
GPU does not move.

    python tools/quick-cpu.py --file plume-shadercache --states 1,0
    python tools/quick-cpu.py --file plume-keyreuse --states 1,0 --rounds 3

States are flipped in A B B A A B ... order. Needs the game in the foreground
on a scene that stands still (or a steady loop); the file is put back after.
"""
import argparse, os, re, subprocess, sys, time

ADB = os.environ.get("ADB") or r"C:\Users\Sidney-PC\AppData\Local\Android\Sdk\platform-tools\adb.exe"
PKG = "org.renut.android"
K = "/sys/class/kgsl/kgsl-3d0"


def adb(*args):
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    return subprocess.run([ADB, *args], capture_output=True, timeout=60, env=env).stdout.decode("utf-8", "replace")


def run_as(cmd):
    return adb("shell", f"run-as {PKG} sh -c '{cmd}'")


def find_thread():
    pid = adb("shell", f"pidof {PKG}:runtime").strip().split()
    if not pid:
        sys.exit("the game's runtime process is not running")
    pid = pid[0]
    out = run_as(f"for t in /proc/{pid}/task/*; do echo $(basename $t) $(cat $t/comm); done")
    for line in out.splitlines():
        m = re.match(r"(\d+) GPU Commands", line.strip())
        if m:
            return pid, m[1]
    sys.exit("no 'GPU Commands' thread found")


def sample(pid, tid):
    out = adb("shell",
              f"dumpsys SurfaceFlinger | grep -A1 'Layer \\[.*SurfaceView\\[org.renut.*BLAST' | head -2; "
              f"cat {K}/clock_mhz {K}/gpu_busy_percentage")
    frame = re.search(r"frame=(\d+)", out)
    nums = re.findall(r"^\s*(\d+)\s*%?\s*$", out, re.M)
    sched = run_as(f"cat /proc/{pid}/task/{tid}/schedstat").split()
    if not frame or len(nums) < 2 or not sched:
        return None
    return dict(t=time.time(), frames=int(frame[1]), run_ns=int(sched[0]), clock=float(nums[-2]), busy=float(nums[-1]))


def window(pid, tid, seconds):
    first = None
    last = None
    clocks, busies = [], []
    end = time.time() + seconds
    while time.time() < end:
        s = sample(pid, tid)
        if s:
            first = first or s
            last = s
            clocks.append(s["clock"])
            busies.append(s["busy"])
        time.sleep(0.5)
    if not first or not last or last["frames"] <= first["frames"]:
        return None
    frames = last["frames"] - first["frames"]
    secs = last["t"] - first["t"]
    return dict(fps=frames / secs, cp_ms=(last["run_ns"] - first["run_ns"]) / 1e6 / frames,
                clock=sum(clocks) / len(clocks), busy=sum(busies) / len(busies))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", default="plume-shadercache")
    ap.add_argument("--states", default="1,0")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--seconds", type=int, default=20)
    ap.add_argument("--settle", type=int, default=6)
    args = ap.parse_args()
    if PKG not in adb("shell", "dumpsys window | grep mCurrentFocus"):
        sys.exit("the game is not in the foreground")
    pid, tid = find_thread()
    original = run_as(f"cat files/{args.file} 2>/dev/null").strip()
    states = args.states.split(",")
    results = {}
    try:
        for r in range(args.rounds):
            for st in states if r % 2 == 0 else states[::-1]:
                run_as(f"echo {st} > files/{args.file}")
                time.sleep(args.settle)
                w = window(pid, tid, args.seconds)
                if not w:
                    print(f"state {st} round {r}: no samples")
                    continue
                results.setdefault(st, []).append(w)
                print(f"state {st} round {r}: fps {w['fps']:5.2f}  command thread {w['cp_ms']:5.2f} ms/frame  "
                      f"clock {w['clock']:4.0f} MHz  busy {w['busy']:3.0f}%", flush=True)
    finally:
        run_as(f"rm -f files/{args.file}" if not original else f"echo {original} > files/{args.file}")
    print()
    base = None
    for st in states:
        ws = results.get(st, [])
        if not ws:
            continue
        cp = sum(w["cp_ms"] for w in ws) / len(ws)
        spread = max(w["cp_ms"] for w in ws) - min(w["cp_ms"] for w in ws)
        fps = sum(w["fps"] for w in ws) / len(ws)
        clk = sum(w["clock"] for w in ws) / len(ws)
        base = base or cp
        print(f"state {st}: {cp:5.2f} ms/frame on the command thread (windows differ by {spread:.2f}), "
              f"{fps:5.2f} fps, {clk:.0f} MHz, {100 * (cp / base - 1):+.1f}% vs state {states[0]}")


if __name__ == "__main__":
    main()
