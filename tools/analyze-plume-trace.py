#!/usr/bin/env python3
"""Summarize one Plume frame trace by render target and shader pair."""

import argparse
import collections
import re


DRAW = re.compile(r"^draw #(?P<id>\d+) vs=(?P<vs>[0-9A-F]+) ps=(?P<ps>[0-9A-F]+) prim=(?P<prim>\d+) n=(?P<n>\d+) mode=(?P<mode>\d+)")
PITCH = re.compile(r"\bpitch=(\d+)")
TARGET = re.compile(r"\brt0=([^ ]+)")
DEPTH = re.compile(r"\bds=([^ ]+)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", help="plume-trace.txt captured from the runtime")
    args = parser.parse_args()

    targets = collections.Counter()
    shaders = collections.Counter()
    target_vertices = collections.Counter()
    runs = []
    previous = None
    run = 0
    total = 0
    shadow_batches = collections.Counter()
    shadow_batch_key = None
    shadow_batch_size = 0
    same_stream_batches = collections.Counter()
    same_stream_sizes = collections.Counter()
    same_stream_key = None
    same_stream_size = 0
    current_draw = None

    def finish_draw() -> None:
        nonlocal shadow_batch_key, shadow_batch_size, same_stream_key, same_stream_size
        if current_draw is None:
            return
        target, shader, details = current_draw
        # A first estimate of batching headroom: all of these guest states
        # must agree before vertex/index buffers could be packed together.
        key = (target, shader, *(details.get(field, "") for field in
                 ("raster", "inputs", "vsc", "viewport", "zviewport")))
        depth_only = not target[1] and target[2] and shader[1] == "0000000000000000"
        if not depth_only or key != shadow_batch_key:
            if shadow_batch_size > 1:
                shadow_batches[shadow_batch_key[1]] += shadow_batch_size - 1
            shadow_batch_key = key if depth_only else None
            shadow_batch_size = 1 if shadow_batch_key else 0
        else:
            shadow_batch_size += 1
        stream_key = (key, details.get("stream0"), details.get("stream10")) if depth_only else None
        if stream_key is None or stream_key != same_stream_key:
            if same_stream_size > 1:
                same_stream_batches[same_stream_key[0][1]] += same_stream_size - 1
                same_stream_sizes[same_stream_size] += 1
            same_stream_key = stream_key
            same_stream_size = 1 if stream_key else 0
        else:
            same_stream_size += 1
    with open(args.trace, encoding="utf-8", errors="replace") as trace:
        for line in trace:
            match = DRAW.match(line)
            if not match:
                if current_draw is not None and line.startswith("  "):
                    detail = line.strip()
                    field = detail.split(" ", 1)[0]
                    if field in ("raster", "inputs", "vsc", "viewport", "zviewport", "stream0", "stream10"):
                        current_draw[2][field] = detail
                continue
            finish_draw()
            total += 1
            pitch = PITCH.search(line)
            color = TARGET.search(line)
            depth = DEPTH.search(line)
            target = (pitch.group(1) if pitch else "?", bool(color), bool(depth))
            shader = (match["vs"], match["ps"])
            current_draw = (target, shader, {})
            key = (target, shader)
            targets[target] += 1
            target_vertices[target] += int(match["n"])
            shaders[shader] += 1
            if key == previous:
                run += 1
            else:
                if previous is not None:
                    runs.append(run)
                previous = key
                run = 1
    if run:
        runs.append(run)
    finish_draw()
    if shadow_batch_size > 1:
        shadow_batches[shadow_batch_key[1]] += shadow_batch_size - 1
    if same_stream_size > 1:
        same_stream_batches[same_stream_key[0][1]] += same_stream_size - 1
        same_stream_sizes[same_stream_size] += 1

    print(f"draws: {total}; unique shader pairs: {len(shaders)}")
    print("target groups (pitch, color, depth):")
    for key, count in targets.most_common():
        print(f"  {key}: {count} draws ({count / total:.1%}), {target_vertices[key]} submitted vertices")
    print("top shader pairs:")
    for key, count in shaders.most_common(12):
        print(f"  vs={key[0]} ps={key[1]}: {count}")
    print(f"contiguous identical target+shader runs: {len(runs)}; max {max(runs, default=0)}; mean {total / len(runs):.2f}" if runs else "no runs")
    print("upper bound for adjacent shadow draw reductions with matching traced state:")
    for shader, saved in shadow_batches.most_common(8):
        print(f"  vs={shader[0]} ps={shader[1]}: up to {saved} fewer draws")
    print(f"  same vertex streams (without repacking): up to {sum(same_stream_batches.values())} fewer draws")
    print(f"  same-stream run sizes: {dict(sorted(same_stream_sizes.items()))}")


if __name__ == "__main__":
    main()
