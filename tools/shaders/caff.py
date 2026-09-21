"""Minimal reader for N&B's big-endian CAFF bundles (07.08.06.0036).

Layout, worked out from x1nixmzeng/project-grabbed and the files themselves:
    0x00  "CAFF" + version[16]
    0x14  header (chunk1 offset, checksum, counts, byte order, section count,
          then chunk1 size at +0x3C+... see below)
    chunk1: section table (33-byte records + names), name pool, per-resource
            table (u32 index, u32 offset, u32 size, u8, u8)
    chunk2: fixups (see Caff.fixups)
    sections, back to back in table order (.data, .texturegpu, .gpu, ...)
"""
import collections
import struct


class Caff:
    def __init__(self, path=None, raw=None):
        self.path = path
        if raw is None:
            raw = open(path, "rb").read()
        self.raw = raw
        assert raw[:4] == b"CAFF", path
        h = struct.unpack_from(">13I4B10I", raw, 20)
        self.chunk1_offset = h[0]
        self.resource_count = h[2]
        # Two fixup lists, each (group count, fixup count); see fixups().
        self.fixup_lists = [(h[6], h[7]), (h[10], h[11])]
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

    def chunk_offset(self, chunk):
        """File offset of a chunk (1-based index into self.chunks)."""
        _, sec, offset, _, _ = self.chunks[chunk - 1]
        return self.sections[sec][0] + offset

    def fixups(self):
        """{source chunk: [(offset in source, target chunk)]}, chunks 1-based.

        chunk2 opens with two fixup lists back to back, sized by the header.
        Each is `groups` records (u32 source chunk, u32 target chunk, u32
        count), then the counts' worth of u32 offsets in group order: the words
        inside the source chunk that hold an offset into the target chunk,
        which the loader turns into an address. The loose CAFFs use only the
        first list; the ones packed in 438cb47c archives put the pointers into
        the pool's .gpu/.gpucached (and a few more) in the second.
        """
        raw, pos = self.raw, self.chunk2_offset
        out = collections.defaultdict(list)
        for group_count, fixup_count in self.fixup_lists:
            groups = [struct.unpack_from(">3I", raw, pos + 12 * i) for i in range(group_count)]
            assert sum(g[2] for g in groups) == fixup_count, self.path
            pos += 12 * group_count
            for source, target, count in groups:
                for offset in struct.unpack_from(f">{count}I", raw, pos):
                    out[source].append((offset, target))
                pos += 4 * count
        return out


def caff_spans(raw):
    """(offset, size) for every CAFF in a packed file, from its own header."""
    spans, i = [], raw.find(b"CAFF")
    while i != -1:
        try:
            h = struct.unpack_from(">13I4B10I", raw, i + 20)
            chunk1_offset, section_count, chunk1_size, chunk2_size = h[0], h[14], h[18], h[23]
            pos, total = i + chunk1_offset, 0
            for _ in range(section_count):
                total += struct.unpack_from(">B8I", raw, pos)[3]
                pos += 33
            size = chunk1_offset + chunk1_size + chunk2_size + total
            if 0 < size <= len(raw) - i:
                spans.append((i, size))
                i = raw.find(b"CAFF", i + size)
                continue
        except Exception:
            pass
        i = raw.find(b"CAFF", i + 4)
    return spans


if __name__ == "__main__":
    import sys
    c = Caff(sys.argv[1])
    print({k: (hex(v[0]), hex(v[1])) for k, v in c.sections.items()})
    print("resources", c.resource_count, "chunks", len(c.chunks),
          "table ends", hex(c.after_chunks), "chunk1 ends", hex(c.chunk1_offset + c.chunk1_size))
