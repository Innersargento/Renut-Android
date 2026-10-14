"""Finds the vertex shaders the native renderer can stand in for.

A vertex shader qualifies when its position is a plain rigid transform:

    rP.xyz = swapFloats(g_SwappedPositions, unpackInput(iPositionN, L), 0).SWZ;
    rP.w = 1.0;
    rA.x = dot(rP.xyzw, cReg(K0).xyzw);   ... .y K1, .z K2, .w K3
    oPos.xyzw = max(rA.xyzw, rA.xyzw);    (or oPos.xyzw = rA.xyzw;)

with no control flow and no other write to rP or rA before oPos. Whatever
else the shader computes (texture coordinates, normals) is irrelevant for a
draw that has no pixel shader: only the position reaches the depth buffer.

Writes src/d3d/native_vs_table.inc: X(hash, location, swizzle, k0, k1, k2, k3, has
texcoord0, its swizzle, has normal, its swizzle), swizzles packed two bits per component
(x=0 y=1 z=2 w=3).

usage: python -I tools/native/scan_native_vs.py <generated/shaders/hlsl> <out.inc>
"""
import os
import re
import sys

HASH = re.compile(r"hash=0x([0-9A-F]{16})")
POS = re.compile(r"^r(\d+)\.xyz = swapFloats\(g_SwappedPositions, unpackInput\(iPosition0, (\d+)\), 0\)\.([xyzw]{3});$")
DOT = re.compile(r"^r(\d+)\.([xyzw]) = dot\(r(\d+)\.xyzw, cReg\((\d+)\)\.xyzw\);$")
OPOS = re.compile(r"^oPos\.xyzw = (?:max\(r(\d+)\.xyzw, r\1\.xyzw\)|r(\d+)\.xyzw);$")
WRITE = re.compile(r"^r(\d+)\.?([xyzw]*) =")
# The first texture coordinate and the normal, which the native view draws with
# (always locations 8 and 5 in the recompiled shaders), and the swizzle the
# shader reads them through.
TEXCOORD = re.compile(r"sintTexcoord\(g_SintTexcoords, swapFloats\(g_SwappedTexcoords, unpackInput\(iTexCoord0, 8\), 0\), 0\)\.([xyzw]{2,4});")
TEXCOORD_LINE = re.compile(r"^r(\d+)\.[xyzw]+ = sintTexcoord\(g_SintTexcoords, swapFloats\(g_SwappedTexcoords, unpackInput\(iTexCoord0, 8\), 0\), 0\)\.[xyzw]+;$")
TEXCOORD_OUT = re.compile(r"^oTexCoord(\d+)\.xy = (?:max\(r(\d+)\.xy, r\2\.xy\)|r(\d+)\.xy);$")
NORMAL = re.compile(r"tfetchR11G11B10\(swapFloats\(g_SwappedNormals, unpackInput\(iNormal0, 5\), 0\)\)\.([xyzw]{3,4})")


def texcoord_output(lines):
    """The interpolant the shader writes the first texture coordinate to as
    fetched (oTexCoordX.xy = rN.xy), 255 if none."""
    reg = None
    for line in lines:
        if reg is None:
            t = TEXCOORD_LINE.match(line)
            if t:
                reg = int(t.group(1))
            continue
        o = TEXCOORD_OUT.match(line)
        if o and int(o.group(2) or o.group(3)) == reg:
            return int(o.group(1))
        w = WRITE.match(line)
        if w and int(w.group(1)) == reg and (not w.group(2) or set(w.group(2)) & set("xy")):
            return 255
    return 255


def packed(swizzle):
    return sum("xyzw".index(c) << (2 * i) for i, c in enumerate(swizzle))


def body(text):
    start = text.find("uint eMWritten = 0;")
    if start < 0:
        return None
    lines = [l.strip() for l in text[start:].splitlines()[1:]]
    return [l for l in lines if l]


def scan(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    if not text.startswith("// vertex shader"):
        return None
    m = HASH.search(text.splitlines()[0])
    if not m or "vfetchLive" in text and "iPosition0" not in text:
        return None
    lines = body(text)
    if not lines:
        return None
    pos_reg = loc = swz = None
    dots = {}
    for line in lines:
        if line.startswith("if") or line.startswith("for") or line.startswith("while") or line in ("{", "}"):
            return None
        if "a0 =" in line or "aL =" in line or "cRegRel" in line and pos_reg is not None:
            pass
        o = OPOS.match(line)
        if o:
            target = int(o.group(1) or o.group(2))
            if pos_reg is None or set(dots.get(target, {})) != set("xyzw"):
                return None
            d = dots[target]
            t = TEXCOORD.search(text)
            n = NORMAL.search(text)
            return (m.group(1), loc, packed(swz), d["x"], d["y"], d["z"], d["w"],
                    1 if t else 0, packed(t.group(1)[:2]) if t else 0,
                    1 if n else 0, packed(n.group(1)[:3]) if n else 0,
                    texcoord_output(lines))
        if line.startswith("oPos"):
            return None
        p = POS.match(line)
        if p:
            if pos_reg is not None:
                return None
            pos_reg, loc, swz = int(p.group(1)), int(p.group(2)), p.group(3)
            continue
        if pos_reg is not None and line == f"r{pos_reg}.w = 1.0;":
            continue
        d = DOT.match(line)
        if d and pos_reg is not None and int(d.group(3)) == pos_reg:
            reg = int(d.group(1))
            if d.group(2) in dots.setdefault(reg, {}):
                return None
            dots[reg][d.group(2)] = int(d.group(4))
            continue
        w = WRITE.match(line)
        if w:
            reg = int(w.group(1))
            if reg == pos_reg:
                return None
            if reg in dots:
                del dots[reg]  # the transform was overwritten
    return None


def main():
    src, out = sys.argv[1], sys.argv[2]
    rows = []
    for name in sorted(os.listdir(src)):
        if name.startswith("vs_") and name.endswith(".hlsl"):
            row = scan(os.path.join(src, name))
            if row:
                rows.append(row)
    with open(out, "w", newline="\n") as f:
        f.write("// Generated by tools/native/scan_native_vs.py: vertex shaders whose position is a\n")
        f.write("// rigid transform of the input position by four float constants.\n")
        f.write("// X(hash, input location, swizzle (2 bits a component), k0, k1, k2, k3,\n")
        f.write("//   reads texcoord0 at 8, its swizzle, reads the normal at 5, its swizzle,\n")
        f.write("//   interpolant it writes texcoord0 to as fetched (255 none))\n")
        for row in rows:
            f.write("X(0x{}ull, {})\n".format(row[0], ", ".join(str(v) for v in row[1:])))
    print(f"{len(rows)} vertex shaders")


if __name__ == "__main__":
    main()
