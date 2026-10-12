"""Extracts the decrypted, decompressed PE image ("basefile") of an Xbox 360 XEX2.

XenosRecomp needs this image to find the shaders that are linked into the
executable (tools/shaders/recompile_shaders.sh reads it as `xex.bin`).

    python tools/xex_basefile.py <default.xex> <out xex.bin>

Handles what this game's executable uses: AES-128-CBC encryption with the file
key wrapped by the retail key, and "basic" compression (data blocks followed by
zero runs). LZX-compressed executables are rejected rather than guessed at.
Uses the `cryptography` package when it is installed (fast); otherwise falls
back to a small pure-Python AES, which takes about half a minute for this image.
"""
import struct
import sys

# Same retail key as external/rexglue-android/src/system/xex_module.cpp.
RETAIL_KEY = bytes([0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                    0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91])
HEADER_FILE_FORMAT = 0x000003FF


def _gf(a, b):
    r = 0
    while b:
        if b & 1:
            r ^= a
        a = (a << 1) ^ (0x11B if a & 0x80 else 0)
        b >>= 1
    return r & 0xFF


def _tables():
    sbox = [0] * 256
    p = q = 1
    while True:  # multiplicative inverse walk, as in the usual S-box derivation
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        x = q ^ (q << 1 | q >> 7) & 0xFF ^ (q << 2 | q >> 6) & 0xFF ^ (q << 3 | q >> 5) & 0xFF ^ (q << 4 | q >> 4) & 0xFF
        sbox[p] = (x ^ 0x63) & 0xFF
        if p == 1:
            break
    sbox[0] = 0x63
    inv = [0] * 256
    for i, v in enumerate(sbox):
        inv[v] = i
    td0 = []
    for x in range(256):
        s = inv[x]
        td0.append((_gf(s, 14) << 24) | (_gf(s, 9) << 16) | (_gf(s, 13) << 8) | _gf(s, 11))
    ror = lambda w, n: ((w >> n) | (w << (32 - n))) & 0xFFFFFFFF
    td1 = [ror(w, 8) for w in td0]
    td2 = [ror(w, 16) for w in td0]
    td3 = [ror(w, 24) for w in td0]
    return sbox, inv, td0, td1, td2, td3


def _decrypt_key_schedule(key, tables):
    sbox, _inv, td0, td1, td2, td3 = tables
    w = list(struct.unpack(">4I", key))
    rcon = 1
    for i in range(4, 44):
        t = w[i - 1]
        if i % 4 == 0:
            t = ((t << 8) | (t >> 24)) & 0xFFFFFFFF
            t = (sbox[t >> 24] << 24) | (sbox[(t >> 16) & 255] << 16) | (sbox[(t >> 8) & 255] << 8) | sbox[t & 255]
            t ^= rcon << 24
            rcon = _gf(rcon, 2)
        w.append(w[i - 4] ^ t)
    rk = []
    for r in range(11):
        rk.extend(w[4 * (10 - r):4 * (10 - r) + 4])
    for i in range(4, 40):  # InvMixColumns on rounds 1..9
        v = rk[i]
        rk[i] = (td0[sbox[v >> 24]] ^ td1[sbox[(v >> 16) & 255]] ^ td2[sbox[(v >> 8) & 255]] ^ td3[sbox[v & 255]])
    return rk


def _cbc_decrypt_pure(key, data):
    tables = _tables()
    inv, td0, td1, td2, td3 = tables[1:]
    rk = _decrypt_key_schedule(key, tables)
    words = struct.unpack(">%dI" % (len(data) // 4), data)
    out = []
    p0 = p1 = p2 = p3 = 0  # zero IV
    for b in range(0, len(words), 4):
        c0, c1, c2, c3 = words[b:b + 4]
        s0 = c0 ^ rk[0]; s1 = c1 ^ rk[1]; s2 = c2 ^ rk[2]; s3 = c3 ^ rk[3]
        k = 4
        for _ in range(9):
            t0 = td0[s0 >> 24] ^ td1[(s3 >> 16) & 255] ^ td2[(s2 >> 8) & 255] ^ td3[s1 & 255] ^ rk[k]
            t1 = td0[s1 >> 24] ^ td1[(s0 >> 16) & 255] ^ td2[(s3 >> 8) & 255] ^ td3[s2 & 255] ^ rk[k + 1]
            t2 = td0[s2 >> 24] ^ td1[(s1 >> 16) & 255] ^ td2[(s0 >> 8) & 255] ^ td3[s3 & 255] ^ rk[k + 2]
            t3 = td0[s3 >> 24] ^ td1[(s2 >> 16) & 255] ^ td2[(s1 >> 8) & 255] ^ td3[s0 & 255] ^ rk[k + 3]
            s0, s1, s2, s3 = t0, t1, t2, t3
            k += 4
        r0 = ((inv[s0 >> 24] << 24) | (inv[(s3 >> 16) & 255] << 16) | (inv[(s2 >> 8) & 255] << 8) | inv[s1 & 255]) ^ rk[40]
        r1 = ((inv[s1 >> 24] << 24) | (inv[(s0 >> 16) & 255] << 16) | (inv[(s3 >> 8) & 255] << 8) | inv[s2 & 255]) ^ rk[41]
        r2 = ((inv[s2 >> 24] << 24) | (inv[(s1 >> 16) & 255] << 16) | (inv[(s0 >> 8) & 255] << 8) | inv[s3 & 255]) ^ rk[42]
        r3 = ((inv[s3 >> 24] << 24) | (inv[(s2 >> 16) & 255] << 16) | (inv[(s1 >> 8) & 255] << 8) | inv[s0 & 255]) ^ rk[43]
        out += (r0 ^ p0, r1 ^ p1, r2 ^ p2, r3 ^ p3)
        p0, p1, p2, p3 = c0, c1, c2, c3
    return struct.pack(">%dI" % len(out), *out)


def _ecb_decrypt_block(key, block):
    # One block, via the CBC routine with a zero IV: D(c) xor 0 == D(c).
    return _cbc_decrypt_pure(key, block)


def _cbc_decrypt(key, data):
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    except ImportError:
        return _cbc_decrypt_pure(key, data)
    d = Cipher(algorithms.AES(key), modes.CBC(bytes(16))).decryptor()
    return d.update(data) + d.finalize()


def extract(xex_bytes):
    magic, _flags, pe_offset, _res, security_offset, count = struct.unpack_from(">4sIIIII", xex_bytes, 0)
    if magic != b"XEX2":
        raise ValueError("Não é um XEX2.")
    optional = {}
    for i in range(count):
        key, value = struct.unpack_from(">II", xex_bytes, 24 + 8 * i)
        optional[key] = value
    if HEADER_FILE_FORMAT not in optional:
        raise ValueError("XEX sem cabeçalho de formato de arquivo.")
    info = optional[HEADER_FILE_FORMAT]
    info_size, encryption, compression = struct.unpack_from(">IHH", xex_bytes, info)
    image_size, = struct.unpack_from(">I", xex_bytes, security_offset + 4)
    file_key = xex_bytes[security_offset + 0x150:security_offset + 0x160]

    data = xex_bytes[pe_offset:]
    if encryption == 1:
        key = _ecb_decrypt_block(RETAIL_KEY, file_key)
        usable = len(data) // 16 * 16
        data = _cbc_decrypt(key, data[:usable]) + data[usable:]
    elif encryption != 0:
        raise ValueError("Tipo de criptografia desconhecido: %d" % encryption)

    if compression == 0:
        image = data[:image_size]
    elif compression == 1:
        image = bytearray()
        position = 0
        for i in range((info_size - 8) // 8):
            block, zeros = struct.unpack_from(">II", xex_bytes, info + 8 + 8 * i)
            image += data[position:position + block]
            image += bytes(zeros)
            position += block
        image = bytes(image)
    else:
        raise ValueError("Compressão do XEX (%d) não suportada por este extrator." % compression)
    if len(image) != image_size or image[:2] != b"MZ":
        raise ValueError("Imagem extraída inválida (tamanho %d, esperado %d)." % (len(image), image_size))
    return image


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    with open(argv[1], "rb") as f:
        image = extract(f.read())
    with open(argv[2], "wb") as f:
        f.write(image)
    print("xex.bin: %d bytes" % len(image))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
