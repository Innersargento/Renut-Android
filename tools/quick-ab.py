#!/usr/bin/env python3
"""Quick A/B of a live Plume switch with the per-pair profiler, hands off.

The game keeps running; the script flips a control file under files/, waits for
the new pipelines to finish compiling (the draw count in the profile settles),
arms the pair profiler (plume-pairprof, mode 2, 3 frames) a few times per state
and prints, for every vertex shader that matters, its GPU time per draw in each
state. A screenshot per state is compared with the first one to show the image
did not change. A whole run is about a minute per state and needs no frame-rate
windows: the per-pair profile is repeatable to a few percent.

    tools/quick-ab.py                      # plume-relconst 0,1,2,0
    tools/quick-ab.py --file plume-fetchspec --modes 0,1,0 --top 12

The scene must stand still while it runs (park the player in Showdown Town).
adb comes from ADB, or the default SDK location.
"""
import argparse, glob, io, os, re, subprocess, sys, time

ADB = os.environ.get("ADB") or r"C:\Users\Sidney-PC\AppData\Local\Android\Sdk\platform-tools\adb.exe"
PKG = "org.renut.android"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HLSL = os.path.join(ROOT, "generated", "shaders", "hlsl")


def adb(*args, binary=False, timeout=60):
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    out = subprocess.run([ADB, *args], capture_output=True, timeout=timeout, env=env)
    return out.stdout if binary else out.stdout.decode("utf-8", "replace")


def run_as(cmd, **kw):
    return adb("shell", f"run-as {PKG} sh -c '{cmd}'", **kw)


def capture(timeout=25):
    """Arm the pair profiler and return the report text ('' on timeout)."""
    run_as("rm -f files/plume-pairprof.txt; echo \"2 3\" > files/plume-pairprof")
    end = time.time() + timeout
    while time.time() < end:
        time.sleep(1.5)
        if "y" in run_as("test -s files/plume-pairprof.txt && echo y"):
            time.sleep(0.5)
            return adb("exec-out", f"run-as {PKG} cat files/plume-pairprof.txt", binary=True).decode("utf-8", "replace")
    return ""


def clock_mhz():
    try:
        return float(adb("shell", "cat /sys/class/kgsl/kgsl-3d0/clock_mhz").strip())
    except ValueError:
        return 0.0


def capture_clocked(timeout=25):
    """capture() plus the GPU clock around it: the profile's times scale with it."""
    before = clock_mhz()
    text = capture(timeout)
    after = clock_mhz()
    return text, (before + after) / 2 if before and after else max(before, after), abs(before - after)


def parse(text):
    hdr, vs, pairs, section = None, {}, {}, "pairs"
    for line in text.splitlines():
        if line.startswith("# per frame"):
            m = re.search(r"([\d.]+) draws, spans ([\d.]+) us", line)
            hdr = (float(m[1]), float(m[2]))
        if line.startswith("# by vertex"):
            section = "vs"
            continue
        if line.startswith("# by pixel"):
            section = "ps"
            continue
        if line.startswith("#"):
            continue
        p = line.split()
        if section == "vs" and len(p) >= 6:
            vs[p[5]] = (float(p[2]), float(p[3]))  # us per frame, draws per frame
        elif section == "pairs" and len(p) >= 11:
            pairs[(p[9], p[10])] = (float(p[2]), float(p[3]))
    return hdr, vs, pairs


def rel_family():
    """Container hashes of the vertex shaders that read constants by a0/aL."""
    found = set()
    for path in glob.glob(os.path.join(HLSL, "vs_*.hlsl")) + glob.glob(os.path.join(HLSL, "xex*.hlsl")):
        with open(path, errors="replace") as f:
            first = f.readline()
            body = f.read()
        if "cRegRel(" in body[body.rfind("void main("):]:
            m = re.search(r"hash=0x([0-9A-F]+)", first)
            if m:
                found.add(m[1])
    return found


def screenshot():
    try:
        from PIL import Image
        import numpy as np
    except ImportError:
        return None
    data = adb("exec-out", "screencap", "-p", binary=True, timeout=30)
    return np.asarray(Image.open(io.BytesIO(data)).convert("RGB"), dtype=np.int16)


def interleave(args, family, baseline, original):
    print(f"interleaved A/B on files/{args.file}: mode 0 against modes {args.interleave}, "
          f"{args.rounds} captures each, {len(family)} vertex shaders in the family")
    try:
        for alt in [int(x) for x in args.interleave.split(",")]:
            run_as(f"echo {10 + alt} > files/{args.file}")
            t0, caps = time.time(), []
            while len(caps) < args.rounds and time.time() - t0 < args.settle:
                text, mhz, swing = capture_clocked()
                hdr, vs, pairs = parse(text)
                if not hdr:
                    continue
                if baseline is None:
                    baseline = hdr[0]
                if steady(hdr[0], baseline, args.tol):
                    # every family shader must show both variants, else the pipelines are still compiling
                    seen = [h for h in vs if int(h, 16) ^ (alt << 60) in {int(f, 16) for f in family}]
                    if seen or not family:
                        caps.append((hdr, vs, mhz))
                        continue
                print(f"    alternate {alt}: capture skipped ({hdr[0]:.0f} draws, pipelines compiling or the scene moved)")
            print(f"  alternate {alt}: {len(caps)} captures after {time.time() - t0:.0f} s")
            fam = {int(f, 16): f for f in family}
            per_vs, pooled = {}, []
            for hdr, vs, mhz in caps:
                a_us = a_dr = b_us = b_dr = 0.0
                for h, (us, dr) in vs.items():
                    v = int(h, 16)
                    if v in fam:
                        base, which = v, "a"
                    elif v ^ (alt << 60) in fam:
                        base, which = v ^ (alt << 60), "b"
                    else:
                        continue
                    row = per_vs.setdefault(base, [0.0, 0.0, 0.0, 0.0])
                    if which == "a":
                        row[0] += us; row[1] += dr; a_us += us; a_dr += dr
                    else:
                        row[2] += us; row[3] += dr; b_us += us; b_dr += dr
                if a_dr and b_dr:
                    pooled.append(((b_us / b_dr) / (a_us / a_dr) - 1) * 100)
            print()
            print(f"mode 0 (a) against mode {alt} (b), same frames, microseconds per draw at about "
                  f"{sum(c[2] for c in caps) / max(len(caps), 1):.0f} MHz:")
            print(f"{'vs':8} {'draws a/b per frame':>20} {'a us':>8} {'b us':>8} {'b vs a':>8}")
            ranked = sorted(per_vs.items(), key=lambda kv: -(kv[1][0] + kv[1][2]))[: args.top]
            for h, (au, ad, bu, bd) in ranked:
                if ad and bd:
                    n = max(len(caps), 1)
                    print(f"{fam[h][:8]:8} {ad / n:9.1f} /{bd / n:9.1f} {au / ad:8.2f} {bu / bd:8.2f} {100 * ((bu / bd) / (au / ad) - 1):+7.1f}%")
            if pooled:
                m = sum(pooled) / len(pooled)
                print(f"pooled family, per capture: " + " ".join(f"{x:+.1f}%" for x in pooled) +
                      f"  -> mean {m:+.1f}% (spread {max(pooled) - min(pooled):.1f} points)")
            print()
    finally:
        run_as(f"echo {original or 0} > files/{args.file}")


def steady(draws, baseline, tol):
    return baseline and abs(draws - baseline) <= tol * baseline


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", default="plume-relconst")
    ap.add_argument("--modes", default="0,1,2,0")
    ap.add_argument("--rounds", type=int, default=2, help="captures per state")
    ap.add_argument("--top", type=int, default=10, help="vertex shaders listed")
    ap.add_argument("--settle", type=int, default=60, help="longest wait for a usable capture per state, seconds")
    ap.add_argument("--tol", type=float, default=0.04, help="draws a frame may differ from the baseline scene")
    ap.add_argument("--clock-tol", type=float, default=0.0,
                    help="a capture counts only at a GPU clock this close to the first one's (cycles a draw do not "
                         "scale with the clock: memory waits do not)")
    ap.add_argument("--wait", type=float, default=0, metavar="MINUTES",
                    help="first wait, up to this long, for the game to be in the foreground and standing still "
                         "(three captures in a row within 2%% of each other, --wait-draws or more), then start")
    ap.add_argument("--wait-draws", type=int, default=3500)
    ap.add_argument("--interleave", default="",
                    help="comma list of alternate modes to compare against mode 0 inside the same frames "
                         "(plume-relconst only: 1,2): the host alternates the two variants draw by draw and marks "
                         "the alternate's vertex shader hash (bit 60/61), so scene and clock cancel out")
    args = ap.parse_args()
    modes = args.modes.split(",")

    if "device" not in adb("devices"):
        sys.exit("no device")
    baseline = None
    if args.wait:
        end, recent = time.time() + args.wait * 60, []
        while True:
            if PKG in adb("shell", "dumpsys window | grep mCurrentFocus"):
                hdr, _, _ = parse(capture(timeout=12))
                if hdr and hdr[0] >= args.wait_draws:
                    recent = (recent + [hdr[0]])[-3:]
                    if len(recent) == 3 and max(recent) - min(recent) <= 0.02 * max(recent):
                        baseline = sorted(recent)[1]
                        print(f"scene steady ({recent}), starting")
                        break
                else:
                    recent = []
            if time.time() > end:
                sys.exit("gave up waiting for a steady scene")
            time.sleep(4)
    focus = adb("shell", "dumpsys window | grep mCurrentFocus")
    if PKG not in focus:
        sys.exit("the game is not in the foreground: " + focus.strip())
    original = run_as(f"cat files/{args.file} 2>/dev/null").strip()
    family = rel_family()
    if args.interleave:
        interleave(args, family, baseline, original)
        return
    print(f"switch files/{args.file}, states {modes}, {args.rounds} captures each; "
          f"{len(family)} vertex shaders read constants by a0/aL")

    results, shots, base_clock = [], [], None
    try:
        for mode in modes:
            run_as(f"echo {mode} > files/{args.file}")
            t0, caps, rejected = time.time(), [], []
            while len(caps) < args.rounds and time.time() - t0 < args.settle:
                text, mhz, swing = capture_clocked()
                hdr, vs, pairs = parse(text)
                if not hdr:
                    continue
                if baseline is None:
                    baseline = hdr[0]
                if base_clock is None and mhz:
                    base_clock = mhz
                ok = (steady(hdr[0], baseline, args.tol) and mhz and swing <= 0.2 * mhz
                      and (not args.clock_tol or abs(mhz - base_clock) <= args.clock_tol * base_clock))
                (caps if ok else rejected).append((hdr, vs, pairs, mhz))
                if not ok and len(rejected) <= 3:
                    print(f"    mode {mode}: capture rejected ({hdr[0]:.0f} draws vs {baseline:.0f}, clock {mhz:.0f} MHz "
                          f"vs {base_clock:.0f}, swing {swing:.0f})")
            shots.append(screenshot())
            results.append((mode, caps, time.time() - t0))
            print(f"  state {mode}: {len(caps)} usable captures after {time.time() - t0:.0f} s")
    finally:
        run_as(f"echo {original or 0} > files/{args.file}")

    def mean_vs(caps):
        # kilocycles: microseconds times the clock of the capture, so a swing of
        # the GPU governor between states does not read as an effect
        acc = {}
        for hdr, vs, pairs, mhz in caps:
            for h, (us, dr) in vs.items():
                a = acc.setdefault(h, [0.0, 0.0])
                a[0] += us * mhz / 1000 / len(caps)
                a[1] += dr / len(caps)
        return acc

    usable = [(m, c) for m, c, _ in results if c]
    if len(usable) < 2:
        sys.exit("INVALID: not enough steady captures (the scene moved or the game left the foreground)")
    tables = [(m, mean_vs(c), sum(x[3] for x in c) / len(c), sum(x[0][0] for x in c) / len(c)) for m, c in usable]
    base = tables[0][1]
    ranked = sorted(base.items(), key=lambda kv: -kv[1][0])
    listed = [h for h, _ in ranked if h in family][: args.top]
    head = " ".join(f"{('st' + m + ' #' + str(i)):>10}" for i, (m, *_r) in enumerate(tables))
    print()
    print("vertex shaders that read constants by a0/aL, kilocycles per draw (draws/frame in the first state):")
    print(f"{'vs':8} {'dr/fr':>6} {head}")
    for h in listed:
        row = []
        for m, t, *_ in tables:
            kc, dr = t.get(h, (0, 0))
            row.append(f"{kc / dr:10.2f}" if dr else f"{'-':>10}")
        print(f"{h[:8]} {base[h][1]:6.1f} " + " ".join(row))
    print()
    print(f"{'':16}{head}")
    fam = lambda t: sum(kc for h, (kc, dr) in t.items() if h in family)
    tot = lambda t: sum(kc for kc, dr in t.values())
    print(f"{'GPU clock MHz':16}" + " ".join(f"{c:10.0f}" for _, _, c, _ in tables))
    print(f"{'draws/frame':16}" + " ".join(f"{d:10.0f}" for _, _, _, d in tables))
    print(f"{'family Mcyc/fr':16}" + " ".join(f"{fam(t) / 1000:10.3f}" for _, t, *_ in tables))
    print(f"{'all draws Mcyc':16}" + " ".join(f"{tot(t) / 1000:10.3f}" for _, t, *_ in tables))
    print(f"{'family share %':16}" + " ".join(f"{100 * fam(t) / tot(t):10.2f}" for _, t, *_ in tables))
    f0 = fam(tables[0][1])
    print()
    for i, (m, t, *_r) in enumerate(tables[1:], 1):
        print(f"state {m} #{i} vs state {tables[0][0]} #0: family {100 * (fam(t) / f0 - 1):+.1f}% in cycles, "
              f"all draws {100 * (tot(t) / tot(tables[0][1]) - 1):+.1f}%")
    if len(tables) > 2 and tables[-1][0] == tables[0][0]:
        drift = 100 * (fam(tables[-1][1]) / f0 - 1)
        print(f"repeatability (first and last state {tables[0][0]}): family {drift:+.1f}% -> "
              f"{'ok' if abs(drift) <= 5 else 'NOT repeatable, do not trust the comparison'}")
    ref = shots[0]
    if ref is not None:
        for i, (m, *_r) in enumerate(tables[1:], 1):
            if shots[i] is None or shots[i].shape != ref.shape:
                continue
            diff = (abs(shots[i] - ref).max(axis=2) > 8).mean() * 100
            print(f"screenshot state {m} #{i} vs state {tables[0][0]} #0: {diff:.2f}% of pixels differ by more than 8/255")


if __name__ == "__main__":
    main()
