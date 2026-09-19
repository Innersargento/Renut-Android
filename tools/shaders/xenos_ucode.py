"""Shader container parsing and a plausibility check for Xenos microcode."""
import struct

CONTAINER = struct.Struct(">9I")  # flags, virtualSize, physicalSize, fieldC, constantTable, definitionTable, shaderOffset, 0, 0


def find_containers(data, start=0, end=None, step_over=True):
    """Yield (offset, fields) for every container header in data[start:end]."""
    end = len(data) if end is None else end
    i = start
    while i + 36 <= end:
        f = CONTAINER.unpack_from(data, i)
        if (f[0] & 0xFFFFFF00) == 0x102A1100 and f[7] == 0 and f[8] == 0 and 0 < f[1] < 0x10000 and f[6] + 24 <= f[1]:
            yield i, f
            i += f[1] if step_over else 4
        else:
            i += 4


def shader_code_range(virtual, fields):
    """(physicalOffset, size) of the microcode, from the Shader struct in the virtual part."""
    so = fields[6]
    phys_off, size = struct.unpack_from(">2I", virtual, so)
    return phys_off, size


# Control-flow opcodes (xenia ucode.h ControlFlowOpcode).
EXEC_OPS = {1, 2, 3, 4, 5, 6, 13, 14}
END_OPS = {2, 4, 6, 14}


def cf_instructions(code):
    """Decode control-flow pairs from the start of the microcode until the first
    exec's target address (CF occupies the first `address` triples)."""
    out = []
    for triple in range(len(code) // 12):
        w0, w1, w2 = struct.unpack_from(">3I", code, triple * 12)
        a = w0 | ((w1 & 0xFFFF) << 32)
        b = (w1 >> 16) | (w2 << 16)
        out.append(a)
        out.append(b)
        if len(out) > 512:
            break
    return out


def plausible_ucode(code):
    """True when `code` starts with a well-formed control-flow program."""
    if len(code) < 12 or len(code) % 12 != 0:
        return False
    slots = len(code) // 12
    cfs = cf_instructions(code)
    first_exec = None
    for cf in cfs:
        op = (cf >> 44) & 0xF
        if op in EXEC_OPS:
            first_exec = cf & 0xFFF
            break
        if op not in (0, 12):  # NOP / ALLOC may precede it
            return False
    if first_exec is None or first_exec == 0 or first_exec >= slots:
        return False
    # Every CF instruction before the ALU/fetch area must decode; one must end.
    ended = False
    for cf in cfs[:first_exec * 2]:
        op = (cf >> 44) & 0xF
        if op in EXEC_OPS:
            addr, count = cf & 0xFFF, (cf >> 12) & 0x7
            if addr < first_exec or addr + count > slots:
                return False
            ended |= op in END_OPS
        elif op in (0, 12, 7, 8, 9, 10, 11):
            continue
        elif op == 15:
            continue
        else:
            return False
        if ended:
            break
    return ended


def exact_ucode(code):
    """plausible_ucode, plus: the exec blocks cover the microcode exactly - the
    highest instruction slot any exec reaches is the last slot of `code`."""
    if not plausible_ucode(code):
        return False
    slots = len(code) // 12
    cfs = cf_instructions(code)
    first_exec = next(cf & 0xFFF for cf in cfs if ((cf >> 44) & 0xF) in EXEC_OPS)
    top = 0
    for cf in cfs[:first_exec * 2]:
        if ((cf >> 44) & 0xF) in EXEC_OPS:
            top = max(top, (cf & 0xFFF) + ((cf >> 12) & 0x7))
    # The compiler pads one slot after the last instruction.
    return top in (slots - 1, slots)


def fetch_slots(code):
    """Instruction slots holding fetch instructions, from the exec sequences."""
    slots = len(code) // 12
    cfs = cf_instructions(code)
    first = next((cf & 0xFFF for cf in cfs if ((cf >> 44) & 0xF) in EXEC_OPS), 0)
    out = []
    for cf in cfs[:first * 2]:
        op = (cf >> 44) & 0xF
        if op not in EXEC_OPS:
            continue
        addr, count, seq = cf & 0xFFF, (cf >> 12) & 0x7, (cf >> 16) & 0xFFF
        for i in range(count):
            if (seq >> (2 * i)) & 1 and addr + i < slots:
                out.append(addr + i)
    return out


def normalized_ucode(code):
    """The microcode with the vfetch fields D3D rewrites at bind time cleared:
    everything but opcode, source/destination registers and source swizzle."""
    b = bytearray(code)
    for slot in fetch_slots(code):
        w0 = struct.unpack_from(">I", b, slot * 12)[0]
        if (w0 & 0x1F) != 0:  # not a vfetch
            continue
        struct.pack_into(">3I", b, slot * 12, w0 & 0xC00FFFFF, 0, 0)
    return bytes(b)
