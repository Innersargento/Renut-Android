"""Prints the PowerPC of recompiled functions from generated/android, with the
callers of each, so the engine can be read without a disassembler.

usage: python -I tools/native/ppc_func.py <generated/android> sub_XXXXXXXX [...]
       python -I tools/native/ppc_func.py <generated/android> --callers sub_XXXXXXXX
"""
import os
import re
import sys

FUNC = re.compile(r"^(?:DEFINE_REX_FUNC|PPC_FUNC_IMPL|PPC_WEAK_FUNC)\(([A-Za-z0-9_]+)\)")


def load(root):
    funcs = {}
    for name in os.listdir(root):
        if not name.endswith(".cpp") or "recomp" not in name:
            continue
        current = None
        for line in open(os.path.join(root, name), encoding="utf-8", errors="replace"):
            m = FUNC.match(line)
            if m:
                current = m.group(1)
                funcs[current] = []
                continue
            if current and line.startswith("\t// "):
                funcs[current].append(line[4:].rstrip())
            elif current and line.startswith("\t") and "(ctx, base);" in line and line.strip().startswith(("sub_", "app")):
                funcs[current].append("    -> " + line.strip())
    return funcs


def callers(funcs, target):
    out = []
    for name, lines in funcs.items():
        if any(l == f"    -> {target}(ctx, base);" for l in lines):
            out.append(name)
    return sorted(out)


def main():
    root = sys.argv[1]
    funcs = load(root)
    if sys.argv[2] == "--callers":
        for t in sys.argv[3:]:
            print(t, "<-", " ".join(callers(funcs, t)))
        return
    for t in sys.argv[2:]:
        lines = funcs.get(t)
        if lines is None:
            print(f"== {t}: not found")
            continue
        n = sum(1 for l in lines if not l.startswith("    ->"))
        print(f"== {t} ({n} instrs) <- {' '.join(callers(funcs, t))}")
        for l in lines:
            if not l.startswith("    ->"):
                print("  " + l)


if __name__ == "__main__":
    main()
