#!/usr/bin/env python3
"""Frame pacing of the running game as the display saw it.

The FPS counter only counts swaps per half second, so a 30 FPS that alternates
16.7 / 50 ms frames still reads 30. This reads what SurfaceFlinger recorded for
the game's layer (`dumpsys SurfaceFlinger --latency`): the time every frame
reached the screen and when its GPU work was ready, and samples the display's
active mode once a second (a variable-refresh panel may switch under the game).

    python tools/pacing-probe.py --seconds 40
    python tools/pacing-probe.py --seconds 40 --label capped

Prints the present intervals in display periods, how far the frame was ready
before its vsync, the display modes seen and the worst stretches. Needs the game
in the foreground on a steady scene. Raw rows go to out/pacing-probe/<label>.csv.
"""
import argparse, collections, os, re, statistics, subprocess, sys, time

ADB = os.environ.get("ADB") or r"C:\Users\Sidney-PC\AppData\Local\Android\Sdk\platform-tools\adb.exe"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "out", "pacing-probe")
PENDING = 9223372036854775807


def adb(*args):
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    return subprocess.run([ADB, *args], capture_output=True, timeout=60, env=env).stdout.decode("utf-8", "replace")


def game_layer():
    for line in adb("shell", "dumpsys SurfaceFlinger --list").splitlines():
        m = re.search(r"(SurfaceView\[org\.renut[^\s]*\(BLAST\)#\d+)", line)
        if m:
            return m[1]
    return None


def active_mode():
    out = adb("shell", "dumpsys SurfaceFlinger | grep -m1 activeMode")
    m = re.search(r"vsyncRate=([\d.]+) Hz", out)
    return float(m[1]) if m else None


def latency(layer):
    out = adb("shell", f"dumpsys SurfaceFlinger --latency '{layer}'").splitlines()
    if not out:
        return None, []
    try:
        period = int(out[0].strip())
    except ValueError:
        return None, []
    rows = []
    for line in out[1:]:
        p = line.split()
        if len(p) == 3:
            desired, actual, ready = (int(x) for x in p)
            if actual != PENDING and actual > 0:
                rows.append((desired, actual, ready))
    return period, rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=40)
    ap.add_argument("--label", default="run")
    ap.add_argument("--wait", type=int, default=0, help="seconds to wait for the game's layer to appear")
    ap.add_argument("--delay", type=int, default=0, help="seconds to wait after the layer appears")
    args = ap.parse_args()

    layer = game_layer()
    waited = 0
    while not layer and waited < args.wait:
        time.sleep(3)
        waited += 3
        layer = game_layer()
    if not layer:
        sys.exit("no org.renut SurfaceView layer: the game has to be on screen")
    print("layer", layer, flush=True)
    if args.delay:
        # Time to get moving: the start of a run is not the scene to measure.
        print(f"measuring in {args.delay} s", flush=True)
        time.sleep(args.delay)

    seen = {}
    modes = collections.Counter()
    mode_log = []
    period = None
    end = time.time() + args.seconds
    next_mode = 0.0
    while time.time() < end:
        p, rows = latency(layer)
        if p:
            period = p
        for r in rows:
            seen[r[1]] = r
        now = time.time()
        if now >= next_mode:
            hz = active_mode()
            modes[hz] += 1
            mode_log.append((round(now - (end - args.seconds), 1), hz))
            next_mode = now + 1.0
        time.sleep(2.0)  # 127 rows hold ~4 s at 30 FPS

    frames = sorted(seen.values(), key=lambda r: r[1])
    if len(frames) < 30:
        sys.exit(f"only {len(frames)} frames recorded")
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, args.label + ".csv"), "w") as f:
        f.write("desired_ns,actual_ns,ready_ns\n")
        for r in frames:
            f.write("%d,%d,%d\n" % r)

    refresh_ms = period / 1e6 if period else 16.667
    gaps = [(frames[i][1] - frames[i - 1][1]) / 1e6 for i in range(1, len(frames))]
    # Drop the hole where a poll missed rows: anything over a quarter second.
    gaps = [g for g in gaps if g < 250]
    span = (frames[-1][1] - frames[0][1]) / 1e9

    print(f"\n{len(frames)} frames over {span:.1f} s -> {len(frames) / span:.2f} FPS; "
          f"layer reports a {refresh_ms:.2f} ms period")
    print("display modes sampled (Hz): " + ", ".join(f"{k}: {v}" for k, v in sorted(modes.items(), key=lambda x: str(x[0]))))
    changes = [(t, hz) for i, (t, hz) in enumerate(mode_log) if i == 0 or hz != mode_log[i - 1][1]]
    if len(changes) > 1:
        print("mode changes: " + ", ".join(f"{t}s -> {hz}" for t, hz in changes))

    print(f"\npresent interval: mean {statistics.mean(gaps):.2f} ms  stdev {statistics.pstdev(gaps):.2f}  "
          f"min {min(gaps):.1f}  max {max(gaps):.1f}")
    buckets = collections.Counter(round(g / refresh_ms) for g in gaps)
    print("interval in display periods:")
    for k in sorted(buckets):
        share = 100 * buckets[k] / len(gaps)
        print(f"  {k:>2} x {refresh_ms:5.2f} ms = {k * refresh_ms:6.1f} ms : {buckets[k]:>5}  {share:5.1f}%  " + "#" * int(share / 2))

    # Judder in the sense a player sees: neighbouring frames of different length.
    jumps = [abs(gaps[i] - gaps[i - 1]) for i in range(1, len(gaps))]
    big = sum(1 for j in jumps if j > 0.75 * refresh_ms)
    print(f"\nneighbouring frames that differ by more than 3/4 of a period: {big} of {len(jumps)} ({100 * big / len(jumps):.1f}%)")

    # How early the frame was ready before the vsync it was shown on.
    slack = [(r[1] - r[2]) / 1e6 for r in frames if r[2] > 0 and 0 <= (r[1] - r[2]) / 1e6 < 100]
    if slack:
        q = sorted(slack)
        print(f"ready -> shown: median {q[len(q) // 2]:.1f} ms, p10 {q[len(q) // 10]:.1f}, p90 {q[len(q) * 9 // 10]:.1f}, "
              f"under 1.5 ms: {100 * sum(1 for s in slack if s < 1.5) / len(slack):.1f}%")

    # Per 2 s window, so a drift that comes and goes shows up.
    print("\nper 2 s window: frames, mean, share of off-pattern intervals")
    t0 = frames[0][1]
    windows = collections.defaultdict(list)
    for i in range(1, len(frames)):
        g = (frames[i][1] - frames[i - 1][1]) / 1e6
        if g < 250:
            windows[int((frames[i][1] - t0) / 2e9)].append(g)
    for w in sorted(windows):
        gs = windows[w]
        odd = 100 * sum(1 for g in gs if abs(g - 33.33) > 0.5 * refresh_ms) / len(gs)
        print(f"  {w * 2:>4}s  {len(gs):>3} frames  mean {statistics.mean(gs):5.1f} ms  off-pattern {odd:5.1f}%")


if __name__ == "__main__":
    main()
