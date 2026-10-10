"""Groups the recompiled pixel shaders into material families: shaders that
are permutations of one Rare uber-shader read the same kinds of things - the
same texture slots, boolean registers and float constants - with features
switched in or out. Prints each family's signature, its size and members, so
the native renderer can port a family once.

usage: python -I tools/native/family_ps.py <generated/shaders/hlsl> [hash...]
  with hashes: show the signature of those shaders and their family.
"""
import collections
import os
import re
import sys

HASH = re.compile(r"hash=0x([0-9A-F]{16})")
FETCH = re.compile(r"tfetch(2D|3D|Cube)\(texture(?:2D|3D|Cube)DescriptorIndex\((\d+)\)")
BOOL = re.compile(r"BOOL_BIT\((\d+)\)")
CREG = re.compile(r"cReg\((\d+)\)")


def features(text):
    body = text[text.find("void main("):]
    fetches = tuple(sorted(set((k, int(s)) for k, s in FETCH.findall(body))))
    bools = tuple(sorted(set(int(b) for b in BOOL.findall(body))))
    consts = tuple(sorted(set(int(c) for c in CREG.findall(body))))
    lines = body.count("\n")
    return fetches, bools, consts, lines


def family_key(f):
    fetches, bools, consts, _ = f
    # The lighting model shows in the constants and booleans it reads; the
    # textures it samples are what its permutations switch.
    return (bools, tuple(c for c in consts if c < 100))


def main():
    src = sys.argv[1]
    shaders = {}
    for name in os.listdir(src):
        if not (name.startswith("ps_") and name.endswith(".hlsl")):
            continue
        text = open(os.path.join(src, name), encoding="utf-8", errors="replace").read()
        m = HASH.search(text.splitlines()[0])
        if m:
            shaders[m.group(1)] = features(text)
    families = collections.defaultdict(list)
    for h, f in shaders.items():
        families[family_key(f)].append(h)
    if len(sys.argv) > 2:
        for want in sys.argv[2:]:
            h = next((k for k in shaders if k.startswith(want.upper())), None)
            if not h:
                print(want, "not found")
                continue
            f = shaders[h]
            key = family_key(f)
            print(f"{h}: {f[3]} lines, fetches {f[0]}, bools {f[1]}, family size {len(families[key])}")
            print(f"   consts {f[2]}")
        return
    ranked = sorted(families.items(), key=lambda kv: -len(kv[1]))
    print(f"{len(shaders)} pixel shaders, {len(families)} families")
    for key, members in ranked[:40]:
        slots = collections.Counter(s for h in members for s in shaders[h][0])
        print(f"{len(members):5d}  bools {list(key[0])[:12]}{'...' if len(key[0]) > 12 else ''}  "
              f"consts {len(key[1])}  e.g. {members[0]}  textures {dict(slots.most_common(6))}")


if __name__ == "__main__":
    main()
