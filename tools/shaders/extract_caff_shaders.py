"""Extract every complete shader container from N&B's bundles.

A model's .data chunk holds the virtual part of each shader (the container
header) in place; the physical part - literals and microcode - lives in the
'pool' resource: .gpu for pixel shaders, .gpucached for vertex shaders. The
model links them with a table of fixups into those two chunks, one pointer per
shader, in the order the headers appear. At load time that pointer becomes the
physical address D3D keeps at object+24 (pixel) / object+32 (vertex). Checked
against the runtime captures: 28,618 extracted containers are byte-identical to
one, and every pairing passes xenos_ucode.exact_ucode.

For each header this writes the container the way DumpShaderObject does
(android/rexglue/d3d_layer.cpp): the virtual bytes followed by the physical
ones, named {vs,ps}_<FNV-1a of the blob>.bin, so a shader already captured at
runtime lands on the same file name.

    python tools/shaders/extract_caff_shaders.py <game Bundle dir> <out dir> [runtime dump dir]

With a runtime dump dir, every extracted shader whose virtual part matches a
captured one is compared byte for byte against it.

Reads the loose CAFFs and the 438cb47c archives (CAFFs back to back). The
0FF512ED files in Bundle/4f are XCTD-compressed and skipped here; run
tools/shaders/decompress_xctd.sh first and point this at its output too.
"""
import collections
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from caff import Caff, caff_spans
import xenos_ucode as ux

HEADER = re.compile(rb"\x10\x2a\x11", re.DOTALL)
# Container flag low byte: pixel shaders are 0x00, vertex shaders set bit 0.
PHYSICAL_SECTION = {False: ".gpu", True: ".gpucached"}


def fnv1a(blob):
    h = 0xCBF29CE484222325
    for b in blob:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def headers(raw, start, end):
    """(offset, fields) of every container header in raw[start:end], in order."""
    out = []
    for m in HEADER.finditer(raw, start, end):
        i = m.start()
        if i & 3 or i + 36 > end:
            continue
        f = ux.CONTAINER.unpack_from(raw, i)
        if f[7] == 0 and f[8] == 0 and 36 <= f[1] < 0x10000 and f[6] + 8 <= f[1] and i + f[1] <= end:
            out.append((i, f))
    return out


class Stats:
    def __init__(self):
        self.c = collections.Counter()
        self.flags = collections.Counter()
        self.unpaired = []


def extract_caff(raw, where, stats, sink):
    c = Caff(where, raw)
    if ".data" not in c.sections:
        return
    fixups = c.fixups()
    pool_chunks = {i + 1 for i, ch in enumerate(c.chunks)
                   if c.names[ch[0] - 1] == "pool" and ch[1] in (".gpu", ".gpucached")}
    for chunk, (resource, sec, offset, size, _) in enumerate(c.chunks, 1):
        if sec != ".data":
            continue
        base = c.chunk_offset(chunk)
        found = headers(raw, base, base + size)
        if not found:
            continue
        stats.c["caff chunks with shaders"] += 1
        pointers = {".gpu": [], ".gpucached": []}
        for at, target in sorted(fixups.get(chunk, ())):
            if target in pool_chunks:
                value = struct.unpack_from(">I", raw, base + at)[0]
                pointers[c.chunks[target - 1][1]].append(c.chunk_offset(target) + value)
        for vertex in (False, True):
            group = [(h, f) for h, f in found if bool(f[0] & 1) == vertex]
            ptrs = pointers[PHYSICAL_SECTION[vertex]]
            stats.c["headers"] += len(group)
            if not ptrs:
                # The standalone aid_{vertex,pixel}shader_*_microcode resources
                # store the container whole: physical part right after the header.
                ptrs = [h + f[1] for h, f in group]
                stats.c["headers with the physical part inline"] += len(group)
            if len(group) != len(ptrs):
                stats.c["headers in chunks with a pointer count mismatch"] += len(group)
                stats.unpaired.append((where, c.names[resource - 1], vertex, len(group), len(ptrs)))
                continue
            for (h, f), physical in zip(group, ptrs):
                stats.flags[f[0]] += 1
                virtual = raw[h:h + f[1]]
                phys = raw[physical:physical + f[2]]
                code_off, code_size = ux.shader_code_range(virtual, f)
                code = phys[code_off:code_off + code_size]
                if len(phys) != f[2] or code_off + code_size > f[2] or not ux.exact_ucode(code):
                    stats.c["paired but microcode check failed"] += 1
                    continue
                stats.c["extracted"] += 1
                sink(vertex, virtual + phys)


def main():
    bundle, out = sys.argv[1], sys.argv[2]
    dump_dir = sys.argv[3] if len(sys.argv) > 3 else None
    os.makedirs(out, exist_ok=True)
    stats = Stats()
    written = {}

    truth = collections.defaultdict(list)  # virtual part -> captured blobs
    if dump_dir:
        for name in os.listdir(dump_dir):
            blob = open(os.path.join(dump_dir, name), "rb").read()
            vsize = struct.unpack_from(">I", blob, 4)[0]
            truth[blob[:vsize]].append((name, blob))
    compared = collections.Counter()
    matched_dumps = set()

    def sink(vertex, blob):
        name = f"{'vs' if vertex else 'ps'}_{fnv1a(blob):016X}.bin"
        if name not in written:
            written[name] = len(blob)
            with open(os.path.join(out, name), "wb") as f:
                f.write(blob)
        if dump_dir:
            vsize = struct.unpack_from(">I", blob, 4)[0]
            captured = truth.get(blob[:vsize])
            if not captured:
                compared["virtual part not captured at runtime"] += 1
            elif any(b == blob for _, b in captured):
                compared["identical to a runtime capture"] += 1
                matched_dumps.update(n for n, b in captured if b == blob)
            elif any(ux.normalized_ucode(code_of(b)) == ux.normalized_ucode(code_of(blob)) for _, b in captured):
                compared["same microcode once vfetch bind fields are cleared"] += 1
            else:
                compared["DIFFERENT from the runtime capture"] += 1

    skipped = collections.Counter()
    for root, _, names in os.walk(bundle):
        for name in sorted(names):
            path = os.path.join(root, name)
            rel = os.path.relpath(path, bundle)
            raw = open(path, "rb").read()
            magic = raw[:4]
            if magic == b"CAFF":
                spans = [(0, len(raw))]
            elif magic == b"\x43\x8c\xb4\x7c":
                spans = caff_spans(raw)
            else:
                skipped[magic.hex()] += 1
                continue
            before = stats.c["extracted"]
            for off, size in spans:
                stats.c["caffs"] += 1
                extract_caff(raw[off:off + size], f"{rel}@{off:#x}", stats, sink)
            print(f"{rel}: {len(spans)} CAFF, {stats.c['extracted'] - before} shaders", flush=True)

    print()
    for k, v in stats.c.items():
        print(f"{k}: {v}")
    print("container flags:", {hex(k): v for k, v in stats.flags.items()})
    print("unique containers written:", len(written),
          f"({sum(1 for n in written if n[:2] == 'vs')} vs, {sum(1 for n in written if n[:2] == 'ps')} ps)")
    print("files skipped by magic:", dict(skipped))
    for row in stats.unpaired[:20]:
        print("  unpaired:", row)
    if dump_dir:
        print("\nagainst the runtime captures:")
        for k, v in compared.items():
            print(f"  {k}: {v}")
        print(f"  runtime captures reproduced exactly: {len(matched_dumps)} of {sum(len(v) for v in truth.values())}")


def code_of(blob):
    f = ux.CONTAINER.unpack_from(blob, 0)
    off, size = ux.shader_code_range(blob[:f[1]], f)
    return blob[f[1] + off:f[1] + off + size]


if __name__ == "__main__":
    main()
