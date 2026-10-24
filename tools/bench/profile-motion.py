#!/usr/bin/env python3
"""Profile of the running game on the phone: where the CPU goes and when frames
are late, for one scene state at a time (parked, then driving).

Per second: frame counter (SurfaceFlinger layer), GPU clock and busy share, the
CPU time of every thread of the game process and the core it last ran on. Then
a simpleperf record of the whole process, reported by thread and by function
(the guest's recompiled functions are `sub_XXXXXXXX` symbols).

    python tools/bench/profile-motion.py --label parked --seconds 25
    python tools/bench/profile-motion.py --label driving --seconds 25
    python tools/bench/profile-motion.py --compare parked driving

Needs the game in the foreground. Output in out/profile-motion/<label>/.
The symbols come from --symdir (an unstripped librenut_runtime.so that matches
the installed build: the obj/arm64-v8a folder of the gradle build that made it).
"""
import argparse, collections, glob, json, os, re, subprocess, sys, time

ADB = os.environ.get("ADB") or r"C:\Users\Sidney-PC\AppData\Local\Android\Sdk\platform-tools\adb.exe"
PKG = "org.renut.android"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "out", "profile-motion")
K = "/sys/class/kgsl/kgsl-3d0"


def adb(*args, timeout=120, binary=False):
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    out = subprocess.run([ADB, *args], capture_output=True, timeout=timeout, env=env).stdout
    return out if binary else out.decode("utf-8", "replace")


def game_pid():
    for name in (PKG + ":runtime", PKG):
        out = adb("shell", f"pidof {name}").strip().split()
        if out:
            return out[0]
    return None


def find_simpleperf():
    hits = glob.glob(os.path.expandvars(r"%LOCALAPPDATA%\Android\Sdk\ndk\*\simpleperf\bin\android\arm64\simpleperf"))
    return sorted(hits)[-1] if hits else None


def tasks_snapshot(pid):
    out = adb("shell", f"run-as {PKG} sh -c 'for f in /proc/{pid}/task/*/stat; do cat $f; done'")
    rows = {}
    for line in out.splitlines():
        m = re.match(r"(\d+) \((.*)\) (\S) (.*)", line.strip())
        if not m:
            continue
        f = m[4].split()
        try:
            rows[int(m[1])] = (m[2], int(f[10]) + int(f[11]), int(f[35]))
        except (IndexError, ValueError):
            pass
    return rows


def sample(pid):
    out = adb("shell", f"dumpsys SurfaceFlinger | grep -A1 'Layer \\[.*SurfaceView\\[org.renut.*BLAST' | head -2; "
                       f"cat {K}/clock_mhz {K}/gpu_busy_percentage")
    m = re.search(r"frame=(\d+)", out)
    nums = re.findall(r"^\s*(\d+)\s*%?\s*$", out, re.M)
    if not m or len(nums) < 2:
        return None
    return time.time(), int(m[1]), float(nums[-2]), float(nums[-1])


def timeline(pid, seconds):
    rows, tasks = [], []
    end = time.time() + seconds
    while time.time() < end:
        s = sample(pid)
        if s:
            rows.append(s)
            tasks.append((s[0], tasks_snapshot(pid)))
        time.sleep(0.2)
    return rows, tasks


def summarize(rows, tasks):
    if len(rows) < 5:
        return None
    t0, f0 = rows[0][:2]
    t1, f1 = rows[-1][:2]
    fps = (f1 - f0) / (t1 - t0)
    # Frame gaps from the counter's steps: the time between two samples that
    # saw it move, divided by the frames in between.
    gaps = []
    for a, b in zip(rows, rows[1:]):
        n = b[1] - a[1]
        if n > 0:
            gaps.extend([(b[0] - a[0]) / n * 1000] * min(n, 3))
    gaps.sort()
    def pct(p):
        return gaps[min(len(gaps) - 1, int(len(gaps) * p))] if gaps else 0
    clock = sum(r[2] for r in rows) / len(rows)
    busy = sum(r[3] for r in rows) / len(rows)
    per = collections.defaultdict(float)
    first, last = tasks[0][1], tasks[-1][1]
    span = tasks[-1][0] - tasks[0][0]
    for tid, (name, j, _) in last.items():
        j0 = first.get(tid, (name, j, 0))[1]
        per[name] += max(0, j - j0) * 0.01 / span
    return dict(fps=fps, p50_ms=pct(0.5), p90_ms=pct(0.9), p99_ms=pct(0.99), worst_ms=gaps[-1] if gaps else 0,
                clock=clock, busy=busy, threads=sorted(per.items(), key=lambda x: -x[1])[:10])


def record(pid, seconds, label, symdir):
    perf = find_simpleperf()
    if not perf:
        print("simpleperf not found in the NDK")
        return None
    adb("push", perf, "/data/local/tmp/simpleperf")
    adb("shell", "chmod 755 /data/local/tmp/simpleperf")
    data = f"/data/local/tmp/perf-{label}.data"
    adb("shell", f"/data/local/tmp/simpleperf record --app {PKG} -f 2000 -g --duration {seconds} -o {data} "
                 f"--exclude-perf", timeout=seconds + 90)
    folder = os.path.join(OUT, label)
    os.makedirs(folder, exist_ok=True)
    local = os.path.join(folder, "perf.data")
    adb("pull", data, local)
    return local


def report(local, symdir, by):
    perf_host = glob.glob(os.path.expandvars("%LOCALAPPDATA%/Android/Sdk/ndk/*/simpleperf/bin/windows/x86_64/simpleperf.exe"))
    if not perf_host:
        return ""
    cmd = [sorted(perf_host)[-1], "report", "-i", local, "--sort", by, "--percent-limit", "0.15", "-n"]
    if symdir:
        # simpleperf looks symbols up by build id under --symfs.
        symfs = os.path.join(OUT, "symfs")
        os.makedirs(symfs, exist_ok=True)
        for so in glob.glob(os.path.join(symdir, "*.so")):
            target = os.path.join(symfs, os.path.basename(so))
            if not os.path.exists(target) or os.path.getsize(target) != os.path.getsize(so):
                import shutil
                shutil.copyfile(so, target)
        cmd += ["--symfs", symfs]
    return subprocess.run(cmd, capture_output=True, timeout=300).stdout.decode("utf-8", "replace")


def log_run(seconds, label):
    """One CSV row a second for a long drive: frames, GPU clock/busy, hottest CPU zone, CPU clocks.
    No simpleperf: the point is to see WHERE and WHEN the frame rate falls, and what the clocks do then."""
    folder = os.path.join(OUT, label)
    os.makedirs(folder, exist_ok=True)
    script = ("dumpsys SurfaceFlinger | grep -A1 'Layer \\[.*SurfaceView\\[org.renut.*BLAST' | head -2; "
              f"echo ==G; cat {K}/clock_mhz {K}/gpu_busy_percentage; "
              "echo ==F; for c in 0 4 7; do cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq; done; "
              "echo ==T; for z in /sys/class/thermal/thermal_zone*; do t=$(cat $z/type 2>/dev/null); "
              "case $t in cpu-*|cpuss-*|gpuss-*|skin*|quiet*) echo $t $(cat $z/temp 2>/dev/null);; esac; done")
    path = os.path.join(folder, "timeline.csv")
    with open(path, "w") as out:
        out.write("t,frames,fps,gpu_mhz,gpu_busy,cpu0_khz,cpu4_khz,cpu7_khz,hot_cpu_c,hot_gpu_c\n")
        end = time.time() + seconds
        last = None
        while time.time() < end:
            text = adb("shell", script)
            m = re.search(r"frame=(\d+)", text)
            if not m:
                time.sleep(0.5)
                continue
            now, frames = time.time(), int(m[1])
            g = text.split("==G")[1].split("==F")[0].split()
            f = text.split("==F")[1].split("==T")[0].split()
            cpu_t = [int(x.split()[-1]) / 1000 for x in text.split("==T")[1].splitlines() if x.startswith(("cpu-", "cpuss-"))
                     and x.split()[-1].lstrip("-").isdigit()]
            gpu_t = [int(x.split()[-1]) / 1000 for x in text.split("==T")[1].splitlines() if x.startswith("gpuss-")
                     and x.split()[-1].lstrip("-").isdigit()]
            fps = (frames - last[1]) / (now - last[0]) if last else 0
            last = (now, frames)
            out.write(f"{now:.1f},{frames},{fps:.1f},{g[0] if g else ''},{g[1] if len(g) > 1 else ''},"
                      f"{f[0] if f else ''},{f[1] if len(f) > 1 else ''},{f[2] if len(f) > 2 else ''},"
                      f"{max(cpu_t) if cpu_t else ''},{max(gpu_t) if gpu_t else ''}\n")
            out.flush()
            time.sleep(0.6)
    print("timeline in", path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", default="run")
    ap.add_argument("--seconds", type=int, default=25)
    ap.add_argument("--symdir", default="")
    ap.add_argument("--compare", nargs=2)
    ap.add_argument("--log", action="store_true", help="long drive: one CSV row a second, no simpleperf")
    args = ap.parse_args()
    if args.compare:
        a, b = (json.load(open(os.path.join(OUT, n, "summary.json"))) for n in args.compare)
        for key in ("fps", "p50_ms", "p90_ms", "p99_ms", "worst_ms", "clock", "busy"):
            print(f"{key:9s} {a[key]:9.2f} {b[key]:9.2f}")
        names = {n for n, _ in a["threads"]} | {n for n, _ in b["threads"]}
        da, db = dict(a["threads"]), dict(b["threads"])
        print("thread cpu (cores):")
        for n in sorted(names, key=lambda n: -max(da.get(n, 0), db.get(n, 0))):
            print(f"  {n:24s} {da.get(n, 0):6.2f} {db.get(n, 0):6.2f}")
        return
    if args.log:
        log_run(args.seconds, args.label)
        return
    pid = game_pid()
    if not pid or PKG not in adb("shell", "dumpsys window | grep mCurrentFocus"):
        sys.exit("the game is not in the foreground")
    folder = os.path.join(OUT, args.label)
    os.makedirs(folder, exist_ok=True)
    print(f"pid {pid}: {args.seconds} s of timeline, then {args.seconds} s of simpleperf (keep the scene going)")
    rows, tasks = timeline(pid, args.seconds)
    summary = summarize(rows, tasks)
    json.dump(summary, open(os.path.join(folder, "summary.json"), "w"), indent=1)
    print(json.dumps(summary, indent=1))
    local = record(pid, args.seconds, args.label, args.symdir)
    if local and os.path.getsize(local) > 0:
        for by in ("comm", "comm,dso,symbol", "dso,symbol"):
            text = report(local, args.symdir, by)
            open(os.path.join(folder, f"report-{by.replace(',', '-')}.txt"), "w", encoding="utf-8").write(text)
        print("reports in", folder)


if __name__ == "__main__":
    main()
