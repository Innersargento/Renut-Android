#!/usr/bin/env python3
"""A/B of two APKs (a change in the recompiled code) on the CPU cost per presented frame, hands off.

quick-cpu.py flips a live switch under files/; a change in the generated code needs another APK, so each
run installs the APK, starts the game (the launcher's "iniciar direto" switch, autoStart), waits for the
title to run steadily, lets it settle, and measures a window: frames presented (SurfaceFlinger), and the
CPU the runtime process spent per frame - all threads, the guest main thread and the command thread - from
/proc/<pid>/task/*/schedstat, next to the frame rate and the GPU clock and busy share.

    python tools/quick-apk-ab.py out/ab/base.apk out/ab/new.apk --rounds 3

Runs go in A B B A A B ... order. The phone has to be unlocked and left alone; installing closes the game.
"""
import argparse, os, re, subprocess, sys, time

ADB = os.environ.get("ADB") or r"C:\Users\Sidney-PC\AppData\Local\Android\Sdk\platform-tools\adb.exe"
PKG = "org.renut.android"
K = "/sys/class/kgsl/kgsl-3d0"


def adb(*args, timeout=180):
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    return subprocess.run([ADB, *args], capture_output=True, timeout=timeout, env=env).stdout.decode("utf-8", "replace")


def run_as(cmd):
    return adb("shell", f"run-as {PKG} sh -c '{cmd}'")


def runtime_pid():
    pids = adb("shell", f"pidof {PKG}:runtime").strip().split()
    return pids[0] if pids else None


def frames():
    out = adb("shell", "dumpsys SurfaceFlinger | grep -A1 'Layer \\[.*SurfaceView\\[org.renut.*BLAST' | head -2")
    m = re.search(r"frame=(\d+)", out)
    return int(m[1]) if m else None


def gpu():
    out = adb("shell", f"cat {K}/clock_mhz {K}/gpu_busy_percentage")
    nums = re.findall(r"^\s*(\d+)\s*%?\s*$", out, re.M)
    return (float(nums[-2]), float(nums[-1])) if len(nums) >= 2 else (0.0, 0.0)


def threads(pid):
    """{tid: (comm, run_ns)} of the runtime process."""
    out = run_as(f"for t in /proc/{pid}/task/*; do echo $(basename $t) $(cut -d' ' -f1 $t/schedstat) $(tr ' ' '_' < $t/comm); done")
    result = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1].isdigit():
            result[p[0]] = (p[2], int(p[1]))
    return result


def foreground():
    return PKG in adb("shell", "dumpsys window | grep mCurrentFocus")


def start_and_wait(apk, ready_timeout=360):
    adb("shell", f"am force-stop {PKG}")
    print("   install:", adb("install", "-r", apk).strip().splitlines()[-1], flush=True)
    adb("shell", "input keyevent KEYCODE_WAKEUP")
    adb("shell", f"monkey -p {PKG} -c android.intent.category.LAUNCHER 1")
    end = time.time() + ready_timeout
    steady = 0
    last = None
    while time.time() < end:
        time.sleep(3)
        pid = runtime_pid()
        f = frames() if pid else None
        if f is not None and last is not None and f - last >= 30:
            steady += 1
            if steady >= 3:
                return pid
        else:
            steady = 0
        last = f
    return None


def measure(pid, settle, seconds):
    time.sleep(settle)
    t0, f0 = time.time(), frames()
    th0 = threads(pid)
    clocks, busies = [], []
    end = time.time() + seconds
    while time.time() < end:
        c, b = gpu()
        clocks.append(c)
        busies.append(b)
        time.sleep(1.0)
    t1, f1 = time.time(), frames()
    th1 = threads(pid)
    if f0 is None or f1 is None or f1 <= f0:
        return None
    n = f1 - f0
    total = sum(max(0, th1[t][1] - th0[t][1]) for t in th1 if t in th0) / 1e6
    def pick(prefix):
        return sum(max(0, th1[t][1] - th0[t][1]) for t in th1 if t in th0 and th1[t][0].startswith(prefix)) / 1e6
    return dict(fps=n / (t1 - t0), total=total / n, main=pick("Main_XThread") / n, cmd=pick("GPU_Commands") / n,
                clock=sum(clocks) / len(clocks), busy=sum(busies) / len(busies))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("apk_a")
    ap.add_argument("apk_b")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--settle", type=int, default=40)
    ap.add_argument("--seconds", type=int, default=60)
    args = ap.parse_args()
    apks = {"A": args.apk_a, "B": args.apk_b}
    order = []
    for r in range(args.rounds):
        order += ["A", "B"] if r % 2 == 0 else ["B", "A"]
    results = {"A": [], "B": []}
    for i, which in enumerate(order, 1):
        print(f"run {i}/{len(order)}: {which} = {os.path.basename(apks[which])}", flush=True)
        pid = start_and_wait(apks[which])
        if not pid:
            print("   the game did not reach a steady frame rate; skipped", flush=True)
            continue
        if not foreground():
            print("   the game is not in the foreground; skipped", flush=True)
            continue
        w = measure(pid, args.settle, args.seconds)
        if not w:
            print("   no samples", flush=True)
            continue
        results[which].append(w)
        print(f"   fps {w['fps']:5.1f}  all threads {w['total']:6.2f} ms/frame  main {w['main']:6.2f}  "
              f"command {w['cmd']:6.2f}  clock {w['clock']:4.0f} MHz  busy {w['busy']:3.0f}%", flush=True)
    print()
    base = None
    for which in "AB":
        ws = results[which]
        if not ws:
            print(f"{which}: no valid runs")
            continue
        def mean(k):
            return sum(w[k] for w in ws) / len(ws)
        total = mean("total")
        base = base or total
        print(f"{which} ({len(ws)} runs): all threads {total:6.2f} ms/frame (runs differ by "
              f"{max(w['total'] for w in ws) - min(w['total'] for w in ws):.2f}), main {mean('main'):6.2f}, "
              f"command {mean('cmd'):6.2f}, {mean('fps'):5.1f} fps, {mean('clock'):.0f} MHz, "
              f"{100 * (total / base - 1):+.1f}% vs A")


if __name__ == "__main__":
    main()
