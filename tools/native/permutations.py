"""Groups the recompiled pixel shaders that are the same permutation of an
uber-shader: identical code once temporaries, material constants and literal
constants are renamed in the order they first appear. The lighting globals the
engine sets per view (c32..c67, c81..c100: sun, ambient, shadow cascades, point
lights) keep their numbers - they are the same registers in every permutation.

For each permutation the native renderer ports once, the members differ only
in which registers hold the material's parameters (and the literal values,
which are checked to be equal) - printed here as the register map.

usage: python -I tools/native/permutations.py <generated/shaders/hlsl> [usage.txt] [--show HASH]
  usage.txt: lines "HASH draws_per_frame" (the native probe's ranking) to weight
  the permutations by how much they draw.
"""
import collections
import os
import re
import sys

HASH = re.compile(r"hash=0x([0-9A-F]{16})")
LITERAL_DEF = re.compile(r"^\s*float4 c(\d+) = asfloat\(uint4\(([^)]*)\)\);", re.M)
TOKEN = re.compile(r"cReg\((\d+)\)|\bc(\d+)\b|\br(\d+)\b")


def is_global(n):
    return 32 <= n <= 67 or 81 <= n <= 100


def normalise(text):
    start = text.find("void main(")
    body = text[start:]
    literals = {int(n): v.replace(" ", "") for n, v in LITERAL_DEF.findall(text)}
    names = {"m": {}, "l": {}, "r": {}}
    material, literal_values = [], []

    def rename(m):
        if m.group(1) is not None:
            n = int(m.group(1))
            if is_global(n):
                return f"cReg({n})"
            if n not in names["m"]:
                names["m"][n] = len(names["m"])
                material.append(n)
            return f"cReg(M{names['m'][n]})"
        if m.group(2) is not None:
            n = int(m.group(2))
            if n not in names["l"]:
                names["l"][n] = len(names["l"])
                literal_values.append(literals.get(n, "?"))
            return f"L{names['l'][n]}"
        n = int(m.group(3))
        if n not in names["r"]:
            names["r"][n] = len(names["r"])
        return f"R{names['r'][n]}"

    lines = []
    for line in body.splitlines():
        line = line.strip()
        if not line or line.startswith("float4 c") and "asfloat" in line:
            continue
        lines.append(TOKEN.sub(rename, line))
    return "\n".join(lines), material, literal_values


def main():
    src = sys.argv[1]
    usage = {}
    show = None
    args = sys.argv[2:]
    if "--show" in args:
        show = args[args.index("--show") + 1].upper()
        args = args[: args.index("--show")]
    if args:
        for line in open(args[0], encoding="utf-8"):
            parts = line.split()
            if len(parts) >= 2 and re.fullmatch(r"[0-9A-F]{16}", parts[0]):
                usage[parts[0]] = float(parts[1].rstrip("/f"))
    groups = collections.defaultdict(list)
    for name in os.listdir(src):
        if not (name.startswith("ps_") and name.endswith(".hlsl")):
            continue
        text = open(os.path.join(src, name), encoding="utf-8", errors="replace").read()
        m = HASH.search(text.splitlines()[0])
        if not m:
            continue
        body, material, literals = normalise(text)
        groups[(body, tuple(literals))].append((m.group(1), material))
    total = sum(usage.values()) or 1
    ranked = sorted(groups.items(), key=lambda kv: (-sum(usage.get(h, 0) for h, _ in kv[1]), -len(kv[1])))
    print(f"{sum(len(v) for v in groups.values())} pixel shaders, {len(groups)} permutations")
    running = 0.0
    for i, ((body, literals), members) in enumerate(ranked[:40]):
        draws = sum(usage.get(h, 0) for h, _ in members)
        running += draws
        lit = "lit" if "BOOL_BIT(243)" in body else "   "
        print(f"{i:3d} {lit} {len(members):4d} shaders  {draws:7.1f} draws/f  cumulative {100 * running / total:5.1f}%  "
              f"e.g. {members[0][0]}  material regs {members[0][1]}")
        if show and any(h.startswith(show) for h, _ in members):
            for h, regs in members:
                print(f"      {h} {regs} {usage.get(h, 0):.1f}/f")


if __name__ == "__main__":
    main()
