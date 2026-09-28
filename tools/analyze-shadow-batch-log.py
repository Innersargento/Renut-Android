#!/usr/bin/env python3
"""Summarize the on/off intervals of a live Plume shadow batch experiment."""

import argparse
import re
import statistics


def field(line: str, name: str) -> int:
    match = re.search(rf"\b{name}=(\d+)", line)
    return int(match.group(1)) if match else 0


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log")
    parser.add_argument("--min-frame", type=int, default=0)
    parser.add_argument("--max-frame", type=int, default=2**63 - 1)
    parser.add_argument("--thread", help="Plume command thread id from the log, e.g. 6462")
    args = parser.parse_args()
    rows = {"on": [], "off": []}
    mode = "off"
    skip_next = False
    with open(args.log, encoding="utf-8", errors="replace") as log:
        for line in log:
            if args.thread and f"[t{args.thread}]" not in line:
                continue
            toggle = re.search(r"shadow indirect batching (on|off)", line)
            if toggle:
                mode = toggle.group(1)
                skip_next = True  # this 300-swap interval straddles the toggle
            if "Renut: Plume frame " not in line:
                continue
            frame = re.search(r"Renut: Plume frame (\d+):", line)
            if not frame or not args.min_frame <= int(frame.group(1)) <= args.max_frame:
                continue
            if skip_next:
                skip_next = False
                continue
            rows[mode].append({name: field(line, name) for name in
                               ("draws", "drawn", "frame_ms", "cp_cpu_ms", "submit_ms")})
            rows[mode][-1]["saved"] = field(line, "saved")
    for mode, group in rows.items():
        print(f"{mode}: {len(group)} complete 300-swap intervals")
        if not group:
            continue
        for name in ("draws", "drawn", "saved", "frame_ms", "cp_cpu_ms", "submit_ms"):
            print(f"  {name}: median {statistics.median(row[name] for row in group):.0f}")


if __name__ == "__main__":
    main()
