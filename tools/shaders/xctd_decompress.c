/*
 * Decompresses Xbox 360 XCTD files (magic 0FF512ED, the console's transparent file
 * compression; Bundle/4f in this game) to plain files, using libmspack's LZX decoder.
 *
 *   xctd_decompress <in> <out>
 *
 * The container, as Microsoft's xcompress reads it (all big endian):
 *
 *   u32 0x0FF512ED, u16 version, u16 reserved, u32 crc, u32 flags
 *   flags: bits 0-3   window = 1 << (n + 15)
 *          bits 4-5   segment size = 0x8000 << n   (compressed bytes, also the segment stride)
 *          bits 6-21  number of segments
 *          bits 22-23 width of a size entry: 0 -> 20 bits, 1 -> 32 bits
 *   a table with the uncompressed size of every segment, bit-packed and padded to 32 bits
 *   the segments: the first starts right after the table, segment i at file offset i * segment size
 *
 * Each segment is an independent LZX stream cut in frames, each frame preceded by its
 * compressed size (u16), or by 0xFF, the uncompressed size (u16) and the compressed size (u16).
 * A frame of odd size is padded with a zero byte for the 16-bit LZX reader (the pad is not
 * in the file). A segment whose first size is zero is stored: 2 zero bytes, then raw data.
 *
 * Build: cc -O2 -I<libmspack>/mspack xctd_decompress.c <libmspack>/mspack/lzxd.c -o xctd_decompress
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mspack.h"
#include "lzx.h"

struct source {
    const uint8_t *buf;
    size_t size, pos, rest;
    int pad;      /* the chunk just finished had an odd size: its last 16-bit word ends in a zero byte */
    size_t chunk; /* size of the chunk being read */
};

struct sink {
    uint8_t *buf;
    size_t size, pos;
};

static int source_read(struct mspack_file *file, void *buffer, int bytes) {
    struct source *s = (struct source *)file;
    if (s->pad) {
        /* The encoder pairs bytes into 16-bit words inside each chunk; an odd chunk ends in half a word. */
        s->pad = 0;
        *(uint8_t *)buffer = 0;
        return bytes > 0 ? 1 : 0;
    }
    if (!s->rest) {
        if (s->pos >= s->size) return 0;
        if (s->buf[s->pos] == 0xFF) {
            if (s->size - s->pos < 5) return 0;
            s->rest = ((size_t)s->buf[s->pos + 3] << 8) | s->buf[s->pos + 4];
            s->pos += 5;
        } else {
            if (s->size - s->pos < 2) return 0;
            s->rest = ((size_t)s->buf[s->pos] << 8) | s->buf[s->pos + 1];
            s->pos += 2;
        }
        if (s->rest > s->size - s->pos) s->rest = s->size - s->pos;
        s->chunk = s->rest;
    }
    if ((size_t)bytes > s->rest) bytes = (int)s->rest;
    if (bytes <= 0) return 0;
    memcpy(buffer, s->buf + s->pos, (size_t)bytes);
    s->pos += (size_t)bytes;
    s->rest -= (size_t)bytes;
    if (!s->rest && (s->chunk & 1)) s->pad = 1;
    return bytes;
}

static int sink_write(struct mspack_file *file, void *buffer, int bytes) {
    struct sink *s = (struct sink *)file;
    if (bytes < 0 || (size_t)bytes > s->size - s->pos) return -1;
    memcpy(s->buf + s->pos, buffer, (size_t)bytes);
    s->pos += (size_t)bytes;
    return bytes;
}

static void *mem_alloc(struct mspack_system *self, size_t bytes) {
    (void)self;
    return calloc(bytes, 1);
}

static void mem_free(void *ptr) { free(ptr); }

static void mem_copy(void *src, void *dst, size_t bytes) { memcpy(dst, src, bytes); }

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* The i-th `width`-bit entry of a big-endian bit stream. */
static uint32_t entry(const uint8_t *table, uint64_t index, unsigned width) {
    uint64_t bit = index * width;
    uint32_t value = 0;
    for (unsigned i = 0; i < width; i++, bit++)
        value = (value << 1) | ((table[bit >> 3] >> (7 - (bit & 7))) & 1);
    return value;
}

static int decode(const uint8_t *data, size_t size, uint8_t **out, size_t *out_size) {
    if (size < 20 || be32(data) != 0x0FF512EDu) return 1;
    uint32_t flags = be32(data + 12);
    uint32_t segments = (flags >> 6) & 0xFFFF;
    unsigned widths[4] = {20, 32, 0, 0};
    unsigned width = widths[(flags >> 22) & 3];
    size_t stride = (size_t)0x8000 << ((flags >> 4) & 3);
    int window_bits = (int)(flags & 0xF) + 15;
    if (!width || !segments || window_bits > 21) return 2;

    size_t table_words = ((size_t)width * segments + 31) >> 5;
    size_t table_bytes = table_words * 4;
    if (16 + table_bytes > size) return 3;
    const uint8_t *table = data + 16;

    uint64_t total = 0;
    for (uint32_t i = 0; i < segments; i++) total += entry(table, i, width);
    if (total > 0x7FFFFFFFull) return 4;
    uint8_t *result = (uint8_t *)malloc(total ? (size_t)total : 1);
    if (!result) return 5;

    struct mspack_system sys;
    memset(&sys, 0, sizeof sys);
    sys.read = (int (*)(struct mspack_file *, void *, int))source_read;
    sys.write = (int (*)(struct mspack_file *, void *, int))sink_write;
    sys.alloc = mem_alloc;
    sys.free = mem_free;
    sys.copy = mem_copy;

    size_t produced = 0;
    for (uint32_t i = 0; i < segments; i++) {
        size_t length = entry(table, i, width);
        size_t begin = i == 0 ? 16 + table_bytes : (size_t)i * stride;
        size_t end = (size_t)(i + 1) * stride;
        if (begin > size) { free(result); return 6; }
        if (end > size) end = size;
        if (length == 0) continue;
        if (end - begin >= 2 && data[begin] == 0 && data[begin + 1] == 0) {
            /* Incompressible segment: a zero chunk size, then the bytes as they are. */
            if (length > end - begin - 2) { free(result); return 6; }
            memcpy(result + produced, data + begin + 2, length);
            produced += length;
            continue;
        }
        struct source src = {data + begin, end - begin, 0, 0, 0, 0};
        struct sink dst = {result + produced, length, 0};
        struct lzxd_stream *lzx = lzxd_init(&sys, (struct mspack_file *)&src, (struct mspack_file *)&dst,
                                            window_bits, 0, 0x8000, (off_t)length, 0);
        if (!lzx) { free(result); return 7; }
        int status = lzxd_decompress(lzx, (off_t)length);
        lzxd_free(lzx);
        if (status != MSPACK_ERR_OK || dst.pos != length) { free(result); return 8; }
        produced += length;
    }
    *out = result;
    *out_size = produced;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <in> <out>\n", argv[0]);
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    if (!in) { perror(argv[1]); return 1; }
    fseek(in, 0, SEEK_END);
    long length = ftell(in);
    fseek(in, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc(length > 0 ? (size_t)length : 1);
    if (!data || fread(data, 1, (size_t)length, in) != (size_t)length) { fprintf(stderr, "%s: read failed\n", argv[1]); return 1; }
    fclose(in);

    uint8_t *out = NULL;
    size_t out_size = 0;
    int status = decode(data, (size_t)length, &out, &out_size);
    free(data);
    if (status) { fprintf(stderr, "%s: not decodable (%d)\n", argv[1], status); return 1; }
    FILE *file = fopen(argv[2], "wb");
    if (!file || fwrite(out, 1, out_size, file) != out_size || fclose(file)) { fprintf(stderr, "%s: write failed\n", argv[2]); return 1; }
    free(out);
    return 0;
}
