"""Minimal reader for N&B's big-endian CAFF bundles (07.08.06.0036).

Layout, worked out from x1nixmzeng/project-grabbed and the files themselves:
    0x00  "CAFF" + version[16]
    0x14  header (chunk1 offset, checksum, counts, byte order, section count,
          then chunk1 size at +0x3C+... see below)
    chunk1: section table (33-byte records + names), name pool, per-resource
            table (u32 index, u32 offset, u32 size, u8, u8)
    chunk2: fixups / dependencies
    sections, back to back in table order (.data, .texturegpu, .gpu, ...)
"""
import struct


class Caff:
    def __init__(self, path):
        self.path = path
        self.raw = raw = open(path, "rb").read()
        assert raw[:4] == b"CAFF", path
        h = struct.unpack_from(">13I4B10I", raw, 20)
        self.chunk1_offset = h[0]
        self.resource_count = h[2]
        self.section_count = h[14]
        self.chunk1_size = h[18]
        self.chunk2_size = h[23]
        self.chunk2_offset = self.chunk1_offset + self.chunk1_size

        pos = self.chunk1_offset
        sizes = []
        for _ in range(self.section_count):
            sizes.append(struct.unpack_from(">B8I", raw, pos)[3])
            pos += 33
        names = []
        for _ in range(self.section_count):
            end = raw.index(b"\0", pos)
            names.append(raw[pos:end].decode())
            pos = end + 1

        self.sections = {}
        base = self.chunk2_offset + self.chunk2_size
        for name, size in zip(names, sizes):
            self.sections[name] = (base, size)
            base += size
        assert base == len(raw), (path, hex(base), hex(len(raw)))

        pool_size = struct.unpack_from(">I", raw, pos)[0]
        pos += 4
        name_offsets = struct.unpack_from(f">{self.resource_count}I", raw, pos)
        pos += 4 * self.resource_count
        pool = raw[pos:pos + pool_size]
        pos += pool_size
        self.names = [pool[o:pool.index(b"\0", o)].decode("latin-1") for o in name_offsets]
        self.after_pool = pos

        # u32 debug-name blob size (0 in retail), then one record per chunk:
        # u32 resource (1-based), u32 offset, u32 size, u8 section (1-based),
        # u8 log2 alignment. A resource has one chunk per section it uses.
        debug = struct.unpack_from(">I", raw, pos)[0]
        pos += 4 + debug
        end = self.chunk1_offset + self.chunk1_size
        self.section_names = names
        self.chunks = []
        while pos + 14 <= end:
            index, offset, size, sec, align = struct.unpack_from(">3I2B", raw, pos)
            if index == 0 or index > self.resource_count or sec == 0 or sec > self.section_count:
                break
            self.chunks.append((index, names[sec - 1], offset, size, align))
            pos += 14
        self.after_chunks = pos

    def section(self, name):
        base, size = self.sections[name]
        return self.raw[base:base + size]


if __name__ == "__main__":
    import sys
    c = Caff(sys.argv[1])
    print({k: (hex(v[0]), hex(v[1])) for k, v in c.sections.items()})
    print("resources", c.resource_count, "chunks", len(c.chunks),
          "table ends", hex(c.after_chunks), "chunk1 ends", hex(c.chunk1_offset + c.chunk1_size))
