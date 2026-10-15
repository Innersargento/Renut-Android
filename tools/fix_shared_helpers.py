#!/usr/bin/env python3
"""Put back the register save/restore helpers in the functions that share registers.

With non_volatile_as_local the code generator prints nothing for a call to
__savegprlr_N / __restgprlr_N / __savefpr_N / __savevmx_N and the like: for a
function that keeps r14-r31 in locals the helpers would only spill zeros. A
function marked share_registers does keep them in ctx, though, so without the
helper its prologue saves nothing and its epilogue restores nothing, and the
caller (which copies its locals back from ctx after the call) reads the clobbered
value. This script puts the helper's own statements - taken from the baseline
output where the helper is a function - in at those call sites, for the functions
named in the config's share_registers list and nowhere else.

    python tools/fix_shared_helpers.py <generated dir> <baseline generated dir> [--check]
"""
import glob, os, re, sys

CALL = re.compile(r"^\t// (bl?) 0x([0-9a-f]{8})\s*$")
HELPER_CALL = re.compile(r"^\t(__(?:save|rest)\w+)\(ctx, base\);\s*$")
FUNC = re.compile(r"^DEFINE_REX_FUNC\((\w+)\) \{")


def read(path):
    return open(path, encoding="utf8", errors="replace", newline="").read()


def helper_tables(baseline):
    """address -> helper name (the function table of the baseline), name -> body statements."""
    addr_of = {}
    bodies = {}
    init = os.path.join(baseline, "renut_android_init.cpp")
    if os.path.isfile(init):
        for m in re.finditer(r"\{\s*0x([0-9A-Fa-f]{8}),\s*(__(?:save|rest)\w+)\s*\}", read(init)):
            addr_of[int(m[1], 16)] = m[2]
    for path in glob.glob(os.path.join(baseline, "renut_android_recomp.*.cpp")):
        lines = read(path).split("\n")
        i = 0
        while i < len(lines):
            line = lines[i]
            m = CALL.match(line.rstrip("\r"))
            if m and i + 1 < len(lines):
                h = HELPER_CALL.match(lines[i + 1].rstrip("\r"))
                if h:
                    addr_of.setdefault(int(m[2], 16), h[1])
            f = FUNC.match(line)
            if f and re.match(r"__(save|rest)", f[1]):
                j = i + 1
                body = []
                while j < len(lines) and not lines[j].startswith("}"):
                    body.append(lines[j])
                    j += 1
                bodies[f[1]] = [b for b in body if "REX_FUNC_PROLOGUE" not in b and not b.strip().startswith("return;")
                                and not b.strip().startswith("// blr")]
                i = j
            i += 1
    return addr_of, bodies


def shared_functions(config_dir):
    names = set()
    for path in glob.glob(os.path.join(config_dir, "*.toml")):
        for line in read(path).split("\n"):
            m = re.match(r"\s*0x([0-9A-Fa-f]{8})\s*=\s*\{(.*)\}", line)
            if m and "share_registers" in m[2] and "true" in m[2]:
                nm = re.search(r'name\s*=\s*"([^"]+)"', m[2])
                names.add(nm[1] if nm else "sub_%s" % m[1].upper())
    return names


def main():
    generated, baseline = sys.argv[1], sys.argv[2]
    check = "--check" in sys.argv
    config_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "config")
    addr_of, bodies = helper_tables(baseline)
    shared = shared_functions(config_dir)
    # the restore helpers' LR load is not needed (the recompiled code returns without it), but is harmless
    total = functions = 0
    for path in glob.glob(os.path.join(generated, "renut_android_recomp.*.cpp")):
        text = read(path)
        if "DEFINE_REX_FUNC(" not in text:
            continue
        lines = text.split("\n")
        out = []
        current = None
        changed = False
        for line in lines:
            f = FUNC.match(line)
            if f:
                current = f[1]
            out.append(line)
            if current in shared:
                m = CALL.match(line.rstrip("\r"))
                if m:
                    helper = addr_of.get(int(m[2], 16))
                    if helper and helper in bodies:
                        marker = "\t// [renut] %s expanded here (share_registers)" % helper
                        out.append(marker)
                        # a block of its own: the body may declare the locals (ea, temp) it uses
                        out.append("	{")
                        out.extend(bodies[helper])
                        out.append("	}")
                        total += 1
                        changed = True
        if changed:
            functions += 1
            if not check:
                open(path, "w", encoding="utf8", newline="").write("\n".join(out))
    print("%s: %d helper calls expanded in %d files (%d shared functions, %d helper addresses)" %
          ("checked" if check else "done", total, functions, len(shared), len(addr_of)))


if __name__ == "__main__":
    main()
