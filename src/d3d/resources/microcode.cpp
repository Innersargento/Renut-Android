// The vertex shader binding and the microcode patching, natively (see microcode.h).
// A transcription of the retail executable's PowerPC, instruction by instruction:
// the comments name the registers the original used so it can be read against the
// disassembly.
#include "d3d/resources/microcode.h"

#include <atomic>
#include <cstring>
#include <utility>

#include "d3d/sync/fence.h"
#include "d3d/device/internal.h"

namespace renut::d3d::port {

namespace {

// ---------------------------------------------------------------------------
// PowerPC rotate and insert, with bits counted from the most significant (IBM)
// ---------------------------------------------------------------------------

constexpr u32 Rotl(u32 value, u32 amount) {
  amount &= 31;
  return amount ? (value << amount) | (value >> (32 - amount)) : value;
}

constexpr u32 Mask(u32 mb, u32 me) {
  const u32 high = 0xFFFFFFFFu >> mb;
  const u32 low = 0xFFFFFFFFu << (31 - me);
  return mb <= me ? (high & low) : (high | low);
}

constexpr u32 Rlwinm(u32 rs, u32 sh, u32 mb, u32 me) { return Rotl(rs, sh) & Mask(mb, me); }

constexpr u32 Rlwimi(u32 ra, u32 rs, u32 sh, u32 mb, u32 me) {
  const u32 mask = Mask(mb, me);
  return (Rotl(rs, sh) & mask) | (ra & ~mask);
}

constexpr u32 Extrwi(u32 rs, u32 n, u32 b) { return Rlwinm(rs, b + n, 32 - n, 31); }
constexpr u32 Insrwi(u32 ra, u32 rs, u32 n, u32 b) { return Rlwimi(ra, rs, 32 - (b + n), b, b + n - 1); }
constexpr u32 Clrlwi(u32 rs, u32 n) { return rs & (0xFFFFFFFFu >> n); }
constexpr u32 Clrlslwi(u32 rs, u32 b, u32 n) { return Rlwinm(rs, n, b - n, 31 - n); }
constexpr u32 Srawi(u32 rs, u32 n) { return u32(i32(rs) >> n); }

// ---------------------------------------------------------------------------
// Guest memory
// ---------------------------------------------------------------------------

struct Guest {
  uint8_t* base;
  u8* Ptr(u32 address) const { return renut::GuestPtr(base, address); }
  u32 Ld(u32 address) const { return LoadBE32(Ptr(address)); }
  u32 Ld16(u32 address) const { return LoadBE16(Ptr(address)); }
  u32 Ld8(u32 address) const { return *Ptr(address); }
  u64 Ld64(u32 address) const { return LoadBE64(Ptr(address)); }
  void St(u32 address, u32 value) const { StoreBE32(Ptr(address), value); }
  void St64(u32 address, u64 value) const { StoreBE64(Ptr(address), value); }
};

// The u16 table the fetch constants' component swizzles are widened through
// (lhzx r, r, 0x82123DF4), and the byte table of bytes per GPU format
// (0x82183AA0, indexed by a format's low 6 bits).
constexpr u32 kSwizzleTable = 0x82123DF4;
constexpr u32 kFormatSizes = 0x82183AA0;
// The counter the declarations' binding ids come from.
constexpr u32 kBindingCounter = 0x82FB0130;

// One vertex fetch constant: three words.
struct FetchWords {
  u32 w0, w1, w2;
};

// The widening the original does through the swizzle table: the table entry for a
// 3-bit index, masked by `mask`, then folded with shifts and ors.
// (Each use below is its own sequence of instructions; they differ in shifts.)

// A declaration's binding id: the counter, incremented until it is neither 0 nor -1.
u32 NextBindingId(const Guest& g) {
  auto* counter = reinterpret_cast<u32*>(g.Ptr(kBindingCounter));
  std::atomic_ref<u32> word(*counter);
  for (;;) {
    u32 raw = word.load(std::memory_order_relaxed);
    for (;;) {
      const u32 value = __builtin_bswap32(raw) + 1;
      if (word.compare_exchange_weak(raw, __builtin_bswap32(value), std::memory_order_acq_rel)) {
        if (value + 1 >= 2)
          return value;
        break;
      }
    }
  }
}

u32 EnsureBindingId(const Guest& g, u32 declaration) {
  if (g.Ld(declaration + 0x30) == 0)
    g.St(declaration + 0x30, NextBindingId(g));
  return g.Ld(declaration + 0x30);
}

}  // namespace

// ---------------------------------------------------------------------------
// 0x821E5218: match the program's vertex fetch constants to a declaration
// ---------------------------------------------------------------------------

void PatchVertexShaderToDeclaration(uint8_t* base, u32 shader, u32 microcode, u32 declaration,
                                    const u8* strides, u32 program) {
  const Guest g{base};
  // r11: the program's container; r27: its vertex fetch count; r30: its table of
  // fetch constant words, one a fetch.
  const u32 container = shader + g.Ld(shader + (program + 0x70) * 8) + 0x368;
  const u32 count = g.Ld(container + 0x1C);
  if (!count || count > 64)
    return;
  const u32 table = container + (g.Ld(container + 0x18) + 9) * 4;
  // r10: the fetch constants the shader was registered with (the originals).
  const u32 originals = shader + program * 0x1A0 + 0x44;
  const u32 declaration_count = g.Ld(declaration + 0x18);  // r24
  const u32 elements = declaration + 0x34;                 // r23, 12 bytes each

  FetchWords out[64];  // count <= 64 above: no heap allocation on the draw path
  u32 r10 = 0;  // a halfword carried from fetch to fetch (the original reads it uninitialised)
  for (u32 k = 0; k < count; ++k) {
    u32 r11 = g.Ld(originals + k * 12);      // w0
    u32 r3 = g.Ld(originals + k * 12 + 4);   // w1
    u32 r31 = g.Ld(originals + k * 12 + 8);  // w2
    const u32 table_word = g.Ld(table + k * 4);  // r5

    // The element of the declaration this fetch is for: same usage and usage index.
    u32 found = declaration_count;
    if (declaration_count) {
      const u32 usage = Extrwi(table_word, 4, 16);
      const u32 usage_index = Extrwi(table_word, 4, 12);
      for (u32 j = 0; j < declaration_count; ++j) {
        if (g.Ld8(elements + j * 12 + 9) == usage && g.Ld8(elements + j * 12 + 0xA) == usage_index) {
          found = j;
          break;
        }
      }
    }

    u32 r9, r8;
    if (found >= declaration_count) {
      // No element: a fetch of the default format (0x5F) with the first stream's stride.
      r11 = Insrwi(r11, 0x5F, 10, 2);
      r10 = Clrlwi(r10, 28);
      const u32 r5 = r3 & 0xBFC6CFFFu;
      const u32 r4 = r31 & 0x80000000u;
      r9 = r11;
      r10 |= 0x9250;
      r11 = r5 | 0x60000u;
      r8 = r4 | strides[0];
    } else {
      const u32 element = elements + found * 12;
      r9 = g.Ld(element + 4);  // the element's type dword
      r10 = Clrlwi(r10, 28);
      u32 r4 = Extrwi(r9, 3, 13);
      u32 r5 = Rlwinm(r9, 22, 26, 28);
      const u32 r21 = Extrwi(r9, 3, 10);
      r5 |= r4;
      r4 = Rlwinm(r9, 0, 16, 21);
      r5 <<= 3;
      const u32 r20 = Rlwinm(r9, 0, 24, 25);
      r5 |= r21;
      r5 <<= 1;
      r5 |= r4;
      r5 <<= 3;
      r10 |= r5;
      r10 = Clrlwi(r10, 16);
      if (r20 == 0x40) {
        r4 = Rlwinm(r10, 0, 16, 18);
        r5 = Clrlwi(r10, 16);
        if (r4 <= 0x6000) {
          r10 = Rlwinm(r5, 0, 16, 18);
          r5 = Clrlwi(r5, 19);
          r10 ^= 0x2000;
          r10 |= r5;
        }
        r4 = Rlwinm(r10, 0, 19, 21);
        r5 = Clrlwi(r10, 16);
        if (r4 <= 0xC00) {
          r10 = Rlwinm(r5, 0, 19, 21);
          r5 &= 0xE3FF;
          r10 ^= 0x400;
          r10 |= r5;
        }
        r4 = Rlwinm(r10, 0, 22, 24);
        r5 = Clrlwi(r10, 16);
        if (r4 <= 0x180) {
          r10 = Rlwinm(r5, 0, 22, 24);
          r5 &= 0xFC7F;
          r10 ^= 0x80;
          r10 |= r5;
        }
        r4 = Rlwinm(r10, 0, 25, 27);
        r5 = Clrlwi(r10, 16);
        if (r4 <= 0x30) {
          r10 = Rlwinm(r5, 0, 25, 27);
          r5 &= 0xFF8F;
          r10 ^= 0x10;
          r10 |= r5;
        }
      }
      r5 = g.Ld16(element);      // the element's stream
      r8 = g.Ld16(element + 2);  // its offset
      const u32 r21b = Clrlslwi(r9, 26, 12);
      u32 r20b = 0x5F - r5;
      r4 = 0x01560056u;
      r9 = Rlwinm(r9, 0, 22, 23);
      r4 = r20b * r4;
      r5 = strides[r5 & 15];
      r20b = r4;
      r20b = Rlwimi(r20b, r4, 11, 8, 12);
      r8 = Rlwinm(r8, 6, 1, 23);
      r9 = r21b | r9;
      r11 = Rlwimi(r11, r20b, 1, 5, 11);
      r8 |= r5;
      r4 = r9 << 4;
      r3 &= 0xBFC0CFFFu;
      r5 = r31 & 0x80000000u;
      r9 = Rlwinm(r11, 0, 5, 1);
      r11 = r4 | r3;
      r8 |= r5;
    }

    // 0x821E5444: the fetch constant's out words, and the swizzle widening.
    u32 r5 = Clrlslwi(r11, 29, 1);
    out[k].w0 = r9;
    r10 = Clrlwi(r10, 16);
    out[k].w2 = r8;
    r10 |= 0xE;
    u32 t = g.Ld16(kSwizzleTable + r5);
    const u32 mask1 = Clrlwi(r10, 16);
    t &= mask1;
    u32 r4 = Clrlwi(t, 16);
    t = Extrwi(t, 13, 16);
    t |= r4;
    t >>= 3;
    t |= r4;
    t >>= 3;
    t |= r4;
    t >>= 3;
    t |= r4;
    r11 = Rlwimi(r11, t, 31, 29, 31);
    u32 r9b = Rlwinm(r11, 30, 28, 30);
    r9b = g.Ld16(kSwizzleTable + r9b);
    r9b &= mask1;
    r5 = Clrlwi(r9b, 16);
    r4 = Extrwi(r9b, 13, 16);
    r9b = Clrlslwi(r9b, 16, 2);
    r4 |= r5;
    r4 >>= 3;
    r4 |= r5;
    r4 >>= 3;
    r5 |= r4;
    r5 >>= 1;
    r9b |= r5;
    r9b = Rlwimi(r9b, r11, 0, 29, 25);
    u32 r11b = Rlwinm(r9b, 27, 28, 30);
    r11b = g.Ld16(kSwizzleTable + r11b);
    r11b &= mask1;
    u32 r8b = Clrlwi(r11b, 16);
    r5 = Extrwi(r11b, 13, 16);
    r11b = Clrlslwi(r11b, 16, 3);
    r5 |= r8b;
    r11b |= r8b;
    r5 >>= 3;
    r11b <<= 2;
    r8b |= r5;
    r8b >>= 1;
    r11b |= r8b;
    r11b = Rlwimi(r11b, r9b, 0, 26, 22);
    u32 r9c = Rlwinm(r11b, 24, 28, 30);
    r9c = g.Ld16(kSwizzleTable + r9c);
    r9c &= mask1;
    r8b = Clrlwi(r9c, 16);
    r5 = Clrlslwi(r9c, 16, 3);
    r9c = Extrwi(r9c, 13, 16);
    r5 |= r8b;
    r9c |= r8b;
    r5 <<= 3;
    r9c >>= 1;
    r8b |= r5;
    r8b <<= 2;
    r9c |= r8b;
    r9c = Rlwimi(r9c, r11b, 0, 23, 19);
    out[k].w1 = r9c;
  }

  // 0x821E5554: order and merge the fetches that share a vertex stream/stride group.
  const u32 table_end = table + count * 4;
  u32 cursor = table;  // r27
  while (cursor < table_end) {
    while (cursor < table_end && !(g.Ld(cursor) & 0x00300000u))
      cursor += 4;
    if (cursor >= table_end)
      break;
    const u32 first = cursor;  // r10
    u32 next = cursor + 4;     // r11
    if (next >= table_end)
      break;
    while (next < table_end && !(g.Ld(next) & 0x00300000u))
      next += 4;
    if (next >= table_end)
      break;

    // The run is the entries from `first` to `next` (the marker that ends it).
    const u32 first_index = (first - table) / 4;
    const u32 between = (next - first) / 4;      // r10
    const u32 run_length = between + 1;           // r5
    FetchWords* run = out + first_index;               // r3
    const u32 terminator = g.Ld(next) & 0x00300000u;  // r7
    cursor = next + 4;                            // r27
    u32 passes = between;                         // r9
    u32 limit = run_length;                       // r4
    u32 end_key = 0xFFFFFFFFu;                    // r29
    if (terminator == 0x00300000u) {
      const u32 end_index = (next - table) / 4;
      const u32 w0 = out[end_index].w0;
      passes = passes ? passes - 1 : 0;
      end_key = Rlwinm(w0, 14, 25, 29) | Extrwi(w0, 2, 5);
      limit = between;
    }

    auto format_size = [&](u32 w1_halfword_source) { return g.Ld8(kFormatSizes + Clrlwi(w1_halfword_source, 26)); };
    // The fetch's second word's low halfword is what the original reads (lhz at +4,
    // which is the high half of the big-endian word).
    auto halfword_at_4 = [](const FetchWords& f) { return f.w1 >> 16; };

    // Sort (bubble) the run: each pass fixes one fetch against all the later ones.
    {
      FetchWords* head_of_pass = run;  // r11
      u32 first_pos = 1;          // r31
      while (passes != 0) {
        if (first_pos < limit) {
          FetchWords* later = head_of_pass + 1;  // r10
          for (u32 n = limit - first_pos; n != 0; --n, ++later) {
            FetchWords& a = *head_of_pass;
            FetchWords& b = *later;
            const u32 key_a = Rlwinm(a.w0, 14, 25, 29) | Extrwi(a.w0, 2, 5);
            const u32 key_b = Rlwinm(b.w0, 14, 25, 29) | Extrwi(b.w0, 2, 5);
            i32 diff = i32(key_b - key_a);
            bool swap = false;
            if (diff == 0) {
              diff = i32(Srawi(a.w2 << 1, 9) - Srawi(b.w2 << 1, 9));
              if (diff == 0)
                diff = i32(format_size(halfword_at_4(a)) - format_size(halfword_at_4(b)));
            } else if (end_key == key_a) {
              swap = true;
            } else if (end_key == key_b) {
              diff = -1;
            }
            if (swap || diff > 0)
              std::swap(a, b);
          }
        }
        --passes;
        ++head_of_pass;
        ++first_pos;
      }
    }

    // Merge neighbours that fit one fetch constant's components.
    FetchWords* head = run;                   // r7
    FetchWords* end = run + run_length;       // r6
    for (FetchWords* it = run + 1; it < end; ++it) {  // r11
      u32 r10 = it->w0;
      u32 r8 = head->w0;
      u32 r9 = Rlwimi(r10, r10, 25, 12, 13);
      u32 r5 = Rlwimi(r8, r8, 25, 12, 13);
      r10 = Rlwinm(r9, 0, 7, 13);
      r9 = Rlwinm(r5, 0, 7, 13);
      if (r9 != r10) {
        head = it;
        continue;
      }
      r10 = Srawi(head->w2 << 1, 9);
      r9 = Srawi(it->w2 << 1, 9);
      if (r10 > r9) {
        head = it;
        continue;
      }
      r5 = format_size(halfword_at_4(*it));
      r10 = r5 - r10;
      r10 += r9;
      if (r10 > 8) {
        head = it;
        continue;
      }
      r9 = Extrwi(r8, 3, 2);
      r10 -= 1;
      if (r9 > r10)
        r10 = r9;
      r8 = Insrwi(r8, r10, 3, 2);
      head->w0 = r8;
      it->w1 |= 0x40000000u;
    }
  }

  // 0x821E57E0: the patched constants go back where the table says.
  for (u32 k = 0; k < count; ++k) {
    const u32 target = microcode + (g.Ld(table + k * 4) & 0xFFF) * 12;
    g.St(target, out[k].w0);
    g.St(target + 4, out[k].w1);
    g.St(target + 8, out[k].w2);
  }
}

// ---------------------------------------------------------------------------
// 0x82656DA0 and its helpers: match the vertex shader's interpolators to a pixel shader
// ---------------------------------------------------------------------------

namespace {

// 0x82656CE8: the high nibble of the pixel shader's interpolator word goes into the
// fetch constants of the vertex shader's interpolator group.
void SetInterpolatorFormat(const Guest& g, u32 vs, u32 microcode, u32 vs_entry, u32 ps_entry) {
  const u32 index = g.Ld16(vs_entry) & 0xFFF;
  u32 entry = vs + (index + Extrwi(g.Ld(vs + 0x14), 5, 22) + g.Ld(vs + 0x1C) + g.Ld(vs + 0x18) + 9) * 4;
  for (;;) {
    const u32 constant = microcode + (g.Ld(entry) & 0xFFF) * 12;
    u32 w0 = g.Ld(constant);
    w0 = Rlwimi(w0, g.Ld(ps_entry), 24, 28, 31);
    g.St(constant, w0);
    if (g.Ld(entry) & 0x1000)
      break;
    entry += 4;
  }
}

// 0x82656C18: an unused interpolator's fetch constants become the dummy {0xC8000000, 0, 0x02000000}.
void ClearInterpolator(const Guest& g, u32 vs, u32 microcode, u32 vs_entry) {
  const u32 index = g.Ld16(vs_entry) & 0xFFF;
  u32 entry = vs + (index + Extrwi(g.Ld(vs + 0x14), 5, 22) + g.Ld(vs + 0x1C) + g.Ld(vs + 0x18) + 9) * 4;
  const u32 w0 = 0xC8000000u;
  const u32 w2 = Rlwimi(0, 1, 25, 3, 7);
  for (;;) {
    const u32 constant = microcode + (g.Ld(entry) & 0xFFF) * 12;
    g.St(constant, w0);
    g.St(constant + 4, 0);
    g.St(constant + 8, w2);
    if (g.Ld(entry) & 0x1000)
      break;
    entry += 4;
  }
}

}  // namespace

void PatchVertexShaderToPixelShader(uint8_t* base, u32 vs, u32 microcode, u32 program_control, u32 ps) {
  const Guest g{base};
  const u32 flags = g.Ld(vs + 0x14);
  if (flags & 0x00040000u)  // already matched
    return;
  if (ps && (g.Ld(ps + 0x14) & 0x00020000u))
    return;
  const u32 vs_outputs = Extrwi(flags, 5, 22);  // r26
  u32 vs_entry = vs + (g.Ld(vs + 0x1C) + g.Ld(vs + 0x18) + 9) * 4;  // r31
  u32 ps_entry = 0;                                                  // r28
  u32 ps_inputs = 0;                                                 // r11 / r25
  u32 ps_reserved = 0;                                               // r24
  if (ps) {
    const u32 ps_flags = g.Ld(ps + 0x14);
    ps_entry = ps + 0x20;
    ps_reserved = Clrlwi(ps_flags, 27);
    ps_inputs = Extrwi(ps_flags, 5, 22);
    if (ps_reserved)
      ps_reserved -= 1;
  }
  u32 consumed = 0;  // r29
  for (u32 remaining = ps_inputs; remaining; --remaining) {
    const u32 wanted = g.Ld8(ps_entry + 3);  // r27
    while (consumed < vs_outputs) {
      if (g.Ld8(vs_entry + 3) >= wanted)
        break;
      ClearInterpolator(g, vs, microcode, vs_entry);
      ++consumed;
      vs_entry += 4;
    }
    if ((g.Ld(vs_entry) ^ g.Ld(ps_entry)) & 0x00000F00u)
      SetInterpolatorFormat(g, vs, microcode, vs_entry, ps_entry);
    vs_entry += 4;
    ps_entry += 4;
    ++consumed;
  }
  if (consumed < vs_outputs) {
    for (u32 rest = vs_outputs - consumed; rest; --rest) {
      ClearInterpolator(g, vs, microcode, vs_entry);
      vs_entry += 4;
    }
  }
  g.St(program_control, Insrwi(g.Ld(program_control), ps_reserved, 4, 8));
}

// ---------------------------------------------------------------------------
// 0x822127F8 and 0x82657048: the binding
// ---------------------------------------------------------------------------

namespace {

constexpr u32 kDeviceStrides = 0x30F0;      // the streams' strides, a byte each (16)
constexpr u32 kDeviceDeclStrides = 0x2E38;  // the strides the declaration was matched to

}  // namespace

bool RebindVertexShader(uint8_t* base, u32 device, u32 shader, u32 declaration, u32 program) {
  const Guest g{base};
  const u32 entry = shader + program * 0x1A0;  // r28
  bool matches = false;
  if (g.Ld(entry + 0x28) == g.Ld(declaration + 0x30)) {
    const u64 low = g.Ld64(device + kDeviceStrides);
    const u64 high = g.Ld64(device + kDeviceStrides + 8);
    const u64 diff = ((g.Ld64(entry + 0x38) ^ high) & g.Ld64(declaration + 0x28)) |
                     ((g.Ld64(entry + 0x30) ^ low) & g.Ld64(declaration + 0x20));
    matches = diff == 0;
  }
  if (!matches) {
    // The old binding is still being read by the GPU: not yet.
    if (const u32 fence = g.Ld(entry + 0x40)) {
      if (FencePending(DeviceAt(base, device), fence))
        return false;
    }
    const u32 instance = shader + g.Ld(shader + (program + 0x70) * 8);
    PatchVertexShaderToDeclaration(base, shader, g.Ld(instance + 0x368) + g.Ld(shader + 0x20), declaration,
                                   g.Ptr(device + kDeviceStrides), program);
    const u32 id = EnsureBindingId(g, declaration);
    g.St(entry + 0x28, id);
    g.St64(entry + 0x30, g.Ld64(device + kDeviceStrides));
    g.St64(entry + 0x38, g.Ld64(device + kDeviceStrides + 8));
  }
  g.St64(device + kDeviceDeclStrides, g.Ld64(device + kDeviceStrides));
  g.St64(device + kDeviceDeclStrides + 8, g.Ld64(device + kDeviceStrides + 8));
  return true;
}

void BindVertexShader(uint8_t* base, u32 shader, u32 declaration, u32 strides) {
  const Guest g{base};
  // The per-stream strides, in units of 4 bytes, as bytes.
  u8 stream_strides[16] = {};
  const u32 last = g.Ld(declaration + 0x1C);
  for (u32 i = 0; i <= last && i < 16; ++i)
    stream_strides[i] = u8(Extrwi(g.Ld(strides + i * 4), 8, 22));
  const u32 programs = (g.Ld(shader + 0x368) & 0x20) ? 2 : 1;
  u32 program_base = shader + 0x28;  // r29
  for (u32 program = 0; program < programs; ++program, program_base += 0x1A0) {
    const u32 instance = shader + g.Ld(shader + (program + 0x70) * 8);
    PatchVertexShaderToDeclaration(base, shader, g.Ld(instance + 0x368) + g.Ld(shader + 0x20), declaration,
                                   stream_strides, program);
    std::memcpy(g.Ptr(program_base + 8), stream_strides, 16);
    g.St(program_base, EnsureBindingId(g, declaration));
  }
  g.St(shader + 0x368, g.Ld(shader + 0x368) | 0x40);
}

}  // namespace renut::d3d::port
