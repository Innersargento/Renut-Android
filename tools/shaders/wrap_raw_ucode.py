"""Wrap raw Xenos vertex shader microcode in a shader container for XenosRecomp.

Direct3D's own shaders (the rectangle-list Clear, the no-op point draws around
fences) live in the executable as bare microcode tables, not as containers,
so the container scan never sees them and the Plume renderer drops their draws.
Without the Clear, the depth the depth-of-field pass restores at the end of a
frame is still there when the first predicated-tiling band of the next frame
draws, and that whole band fails the depth test.

The container declares no vertex elements, so every vfetch becomes a manual
fetch that decodes stream, format, stride and offset from the instruction, and
one interpolator per exported register (COLOR0 first, which is what Direct3D's
Clear pixel shader reads, then TEXCOORD0...).

    python wrap_raw_ucode.py <out dir> vs_<hash>.ucode...

The .ucode files are the guest (big-endian) microcode the renderer dumps to
files/plume-miss. Put the output next to xex.bin in recompile_shaders.sh's
input directory.
"""
import os
import struct
import sys

FLAGS_VERTEX = 0x102A1101
HEADER = struct.Struct(">9I")
SHADER = struct.Struct(">9I")  # physicalOffset, size, 8, C, 10, interpolatorInfo, 18, elementCount, 20

USAGE_TEXCOORD = 5
USAGE_COLOR = 10


def exported_registers(code):
    """Interpolator registers (0..15) written by ALU exports, from every exec's
    instructions; fetches are skipped by the exec's sequence bits."""
    dwords = struct.unpack(">%dI" % (len(code) // 4), code)
    cf = []
    for t in range(len(dwords) // 3):
        w0, w1, w2 = dwords[t * 3:t * 3 + 3]
        cf.append(w0 | ((w1 & 0xFFFF) << 32))
        cf.append((w1 >> 16) | (w2 << 16))
    regs = set()
    first = None
    for ins in cf:
        opcode = (ins >> 44) & 0xF
        if opcode not in (1, 2, 3, 4, 5, 6):
            continue
        address = ins & 0xFFF
        count = (ins >> 12) & 0x7
        sequence = (ins >> 16) & 0xFFF
        first = address if first is None else min(first, address)
        for k in range(count):
            if (sequence >> (k * 2)) & 1:
                continue  # fetch
            w0 = dwords[(address + k) * 3]
            if not (w0 >> 15) & 1:
                continue  # not an export
            # The recompiler maps an export's vector and scalar destinations
            # even when their write masks are empty.
            for dest in (w0 & 0x3F, (w0 >> 8) & 0x3F):
                if dest < 16:
                    regs.add(dest)
        if opcode in (2, 4, 6) or (first is not None and len(regs) > 16):
            break
    return sorted(regs)


def wrap(code):
    regs = exported_registers(code)
    interpolators = []
    for i in range(max(regs) + 1 if regs else 0):
        usage, index = (USAGE_COLOR, 0) if i == 0 else (USAGE_TEXCOORD, i - 1)
        interpolators.append(index | (usage << 4) | (i << 8))
    table = [0x290] + interpolators  # field18 word, no vertex elements
    shader_offset = HEADER.size
    virtual_size = shader_offset + SHADER.size + 4 * len(table)
    header = HEADER.pack(FLAGS_VERTEX, virtual_size, len(code), 0, 0, 0, shader_offset, 0, 0)
    shader = SHADER.pack(0, len(code), 0x10000, 0, 0, len(interpolators) << 5, 1, 0, 0)
    return header + shader + struct.pack(">%dI" % len(table), *table) + code


def main():
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)
    for path in sys.argv[2:]:
        code = open(path, "rb").read()
        name = os.path.splitext(os.path.basename(path))[0]
        with open(os.path.join(out_dir, name + ".bin"), "wb") as f:
            f.write(wrap(code))
        print(name, "exports", exported_registers(code))


if __name__ == "__main__":
    main()
