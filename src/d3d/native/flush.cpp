// The state flush: what Direct3D's SetPending_* functions send, redone from the device.
#include "internal.h"

namespace renut::native_d3d {

void RenderStates(Device& d, uint64_t mask, uint32_t first, uint32_t shadow) {
  while (mask) {
    const uint32_t skip = uint32_t(__builtin_clzll(mask));
    mask <<= skip;
    first += skip;
    shadow += skip * 4;
    const uint32_t run = ~mask ? uint32_t(__builtin_clzll(~mask)) : 64;
    d.out.RegsFromGuest(d.base, first, shadow, run);
    first += run;
    shadow += run * 4;
    mask = run >= 64 ? 0 : mask << run;
  }
}

#if defined(_WIN32)
bool GuestCommitted(uint32_t address) {
  auto* memory = rex::system::kernel_memory();
  if (!memory) return false;
  const auto* heap = memory->LookupHeap(address);
  if (!heap) return false;
  rex::memory::HeapAllocationInfo info{};
  return const_cast<rex::memory::BaseHeap*>(heap)->QueryRegionInfo(address, &info) &&
         (info.state & rex::memory::kMemoryAllocationCommit);
}
#endif

void Surfaces(Device& d, uint64_t mask) {
  static constexpr uint32_t kSlotOfRegister[6] = {0, 0, 4, 1, 2, 3};
  uint32_t slots = 0;
  for (uint32_t r = 1; r < 6; ++r) {
    if ((mask >> (63 - r)) & 1) slots |= 1u << kSlotOfRegister[r];
  }
  if (!slots) return;
  List& list = d.out.list();
  list.words.push_back(kOpSurfaces | (slots << 8));
  for (uint32_t slot = 0; slot < 5; ++slot) {
    if (!((slots >> slot) & 1)) continue;
    uint32_t surface = U32(d.base, d.dev + (slot < 4 ? 12440 + slot * 4 : 12456));
    if (surface && !GuestCommitted(surface)) surface = 0;
    list.words.push_back(surface);
    for (uint32_t at : {24u, 28u, 36u}) list.words.push_back(surface ? U32(d.base, surface + at) : 0);
  }
}

void AluConstants(Device& d, uint64_t mask, uint32_t first, uint32_t shadow) {
  while (mask) {
    const uint32_t skip = uint32_t(__builtin_clzll(mask));
    mask <<= skip;
    first += skip * 16;
    shadow += skip * 64;
    const uint32_t run = ~mask ? uint32_t(__builtin_clzll(~mask)) : 64;
    d.out.RegsFromGuest(d.base, first, shadow, run * 16);
    first += run * 16;
    shadow += run * 64;
    mask = run >= 64 ? 0 : mask << run;
  }
}

void FetchConstants(Device& d, uint64_t mask) {
  uint32_t slot = 0, sent = 0;
  while (mask) {
    const uint32_t skip = uint32_t(__builtin_clzll(mask));
    mask <<= skip;
    slot += skip;
    const uint32_t run = ~mask ? uint32_t(__builtin_clzll(~mask)) : 64;
    d.out.RegsFromGuest(d.base, 0x4800 + slot * 6, d.dev + kFetchShadow + slot * 24, run * 6);
    for (uint32_t i = slot; i < slot + run && i < 32; ++i) sent |= 1u << i;
    slot += run;
    mask = run >= 64 ? 0 : mask << run;
  }
  if (sent >> 26) Streams(d, sent);
}

void Streams(Device& d, uint32_t slots) {
  uint32_t mask = 0;
  for (uint32_t s = 0; s < 18; ++s) {
    if ((slots >> ((95 - s) / 3)) & 1) mask |= 1u << s;
  }
  if (!mask) return;
  List& list = d.out.list();
  list.words.push_back(kOpStreams | (mask << 8));
  for (uint32_t s = 0; s < 18; ++s) {
    if (!((mask >> s) & 1)) continue;
    uint32_t vb = U32(d.base, d.dev + 12460 + s * 4);
    // Streams 16 and 17 ride along with the fetch slot they share with stream
    // 15, and what sits past the device's sixteen sources is not an object.
    if (vb && !GuestCommitted(vb)) vb = 0;
    const uint32_t address = vb ? U32(d.base, vb + 24) & ~3u : 0;
    list.words.push_back(address ? Phys(address) : 0);
    list.words.push_back(address ? U32(d.base, vb + 28) & 0x03FFFFFCu : 0);
  }
}

void Indices(Device& d, uint32_t ib) {
  List& list = d.out.list();
  if (ib && !GuestCommitted(ib)) ib = 0;
  const uint32_t address = ib ? U32(d.base, ib + 24) : 0;
  const uint32_t base = address ? Phys(address) : 0, size = address ? U32(d.base, ib + 28) : 0;
  if (list.indices_named && list.indices_base == base && list.indices_size == size) return;
  list.indices_named = true;
  list.indices_base = base;
  list.indices_size = size;
  list.words.push_back(kOpIndices);
  list.words.push_back(base);
  list.words.push_back(size);
}

void ClipPlanes(Device& d, uint64_t mask) {
  d.out.Reg(0x2007, U32(d.base, d.dev + kRegs2000 + 7 * 4));
  uint32_t plane = 0;
  while (mask) {
    const uint32_t skip = uint32_t(__builtin_clzll(mask));
    mask <<= skip;
    plane += skip;
    const uint32_t run = ~mask ? uint32_t(__builtin_clzll(~mask)) : 64;
    d.out.RegsFromGuest(d.base, 0x2388 + plane * 4, d.dev + kClipPlanes + plane * 16, run * 4);
    plane += run;
    mask = run >= 64 ? 0 : mask << run;
  }
}

void LiteralConstants(Device& d, uint32_t header, uint32_t physical) {
  const uint32_t table = U32(d.base, header + 20);
  if (!table) return;
  const uint32_t t = header + table;
  uint32_t entry = t + 20;
  const uint32_t end = U32(d.base, t + 16) + entry;
  while (entry < end) {
    const uint32_t constant = U16(d.base, entry);
    const uint32_t dwords = U16(d.base, entry + 2);
    if (!dwords) break;
    const uint32_t source = U32(d.base, entry + 4) + physical;
    entry += 8;
    d.out.RegsFromGuest(d.base, 0x4000 + constant * 4, Cpu(Phys(source)), dwords);
    g_counts.literal_blocks.fetch_add(1, std::memory_order_relaxed);
  }
}

bool BindingMatches(Device& d, uint32_t shader, uint32_t declaration, uint32_t binding) {
  d.ctx.r3.u64 = d.dev;
  d.ctx.r4.u64 = shader;
  d.ctx.r5.u64 = declaration;
  d.ctx.r6.u64 = binding;
  sub_822127F8(d.ctx, d.base);
  return d.ctx.r3.u32 != 0;
}

void PatchAndLoad(Device& d, uint32_t match_ps, uint32_t shader, uint32_t program_cntl, uint32_t declaration,
                  uint32_t vs_info, uint32_t ps_info, uint32_t binding) {
  const uint32_t info = U32(d.base, shader + (binding + 112) * 8) + shader;
  const uint32_t bytes = U32(d.base, info + 876);
  const uint32_t dwords = bytes >> 2;
  const uint32_t copy = ScratchAlloc(bytes);
  if (!copy) return;
  const uint32_t source = Cpu(Phys(U32(d.base, info + 872) + U32(d.base, shader + 32)));
  std::memcpy(GuestPtr(d.base, copy), GuestPtr(d.base, source), bytes);
  if (match_ps && (U8(d.base, vs_info + 8) & 7) != 7) {
    d.ctx.r3.u64 = vs_info;
    d.ctx.r4.u64 = copy;
    d.ctx.r5.u64 = program_cntl;
    d.ctx.r6.u64 = ps_info;
    sub_82656DA0(d.ctx, d.base);
  }
  const uint8_t flags = U8(d.base, d.dev + 10942);
  S8(d.base, d.dev + 10942, uint8_t((flags & 0x7F) | (match_ps ? 0x80 : 0)));
  d.ctx.r3.u64 = shader;
  d.ctx.r4.u64 = copy;
  d.ctx.r5.u64 = declaration;
  d.ctx.r6.u64 = d.dev + 12528;
  d.ctx.r7.u64 = binding;
  sub_821E5218(d.ctx, d.base);
  S64(d.base, d.dev + 11840, U64(d.base, d.dev + 12536));
  S64(d.base, d.dev + 11832, U64(d.base, d.dev + 12528));
  d.out.Shader(0, Phys(copy), dwords, 0);
  g_counts.patched_loads.fetch_add(1, std::memory_order_relaxed);
}

uint64_t PendingHiZEnable(Device& d, uint64_t mask) {
  uint8_t* base = d.base;
  const uint32_t dev = d.dev;
  uint32_t ps_kills = 0;
  if (const uint32_t ps = U32(base, dev + kPixelShader)) {
    const uint32_t info = U32(base, ps + 64) + ps;
    ps_kills = (U32(base, info + 48) >> 27) & 1;
  }
  const uint32_t depth = U32(base, dev + kRegs2200);  // RB_DEPTHCONTROL
  const uint32_t r7 = depth >> 1;
  const uint32_t hiz = U32(base, dev + 10560);
  uint32_t r11 = ((uint32_t(U8(base, dev + 10942)) >> 5) & r7);
  r11 = (r11 & 1) | (hiz & ~1u);
  S32(base, dev + 10560, r11);
  const uint32_t stencil = (U32(base, dev + 11852) >> 20) & 0xFFF;
  uint32_t enable;
  if (!(stencil & 2)) {
    enable = stencil & 1;
  } else {
    const uint32_t zfunc = (depth >> 4) & 7;
    bool ok;
    if (r11 & 0x10) {
      ok = zfunc == 4 || zfunc == 6 || zfunc == 2;
    } else {
      ok = zfunc == 1 || zfunc == 3 || zfunc == 2;
    }
    enable = r11 & ~ps_kills & r7 & (ok ? 1u : 0u) & 1u;
    if (depth & 1) {
      // Stencil on: only while the stencil ops (front, and back when two
      // sided) leave the buffer alone.
      enable &= ((depth & 0x00003800u) ? 0 : 1) & ((depth & 0x000E0000u) ? 0 : 1);
      if (depth & 0x80) enable &= ((depth & 0xE0000000u) ? 0 : 1) & ((depth & 0x03800000u) ? 0 : 1);
    }
  }
  uint32_t v = U32(base, dev + 10560);
  v = (v & ~2u) | ((enable & 1) << 1);
  S32(base, dev + 10560, v);
  const uint8_t b41 = U8(base, dev + 10941);
  if (v & 2) {
    if ((b41 & 4) && (U32(base, dev + 13504) == 0 || U32(base, dev + 13512) == 1)) {
      S8(base, dev + 10941, b41 & 0xFB);
      g_counts.hiz_events.fetch_add(1, std::memory_order_relaxed);
    }
  } else if ((r7 & 1) && (depth & 4)) {
    const uint32_t zfunc = (depth >> 4) & 7;
    bool set = zfunc == 7 || zfunc == 5;
    if (!set) set = (v & 0x10) ? (zfunc == 1 || zfunc == 3) : (zfunc == 4 || zfunc == 6);
    if (set) S8(base, dev + 10941, b41 | 4);
  }
  return mask | 0x100;
}

uint64_t PendingShaders(Device& d, uint64_t r20) {
  uint8_t* base = d.base;
  const uint32_t dev = d.dev;
  int32_t r15 = -1, r19 = -1;
  if (r20 & 0x20000) r20 = PendingHiZEnable(d, r20);
  const uint32_t vs = U32(base, dev + kVertexShader);
  const uint32_t decl = U32(base, dev + kDeclaration);
  const uint32_t ps = U32(base, dev + kPixelShader);
  if (!vs) return r20;
  uint32_t r24 = 0;
  uint32_t r25 = U32(base, vs + 896) + vs + 872;
  uint32_t r28 = 0, r18 = 0, r17 = 0, r27 = 0;
  if (!ps) {
    const uint8_t b43 = U8(base, dev + 10943);
    if (b43 & 1) {
      d.out.Reg(0x05C8, 0x20000);
      S8(base, dev + 10943, b43 & 0xFE);
    }
    if (U32(base, vs + 872) & 0x20) {
      r24 = 1;
      r25 = U32(base, vs + 904) + vs + 872;
    }
    uint32_t mc = U32(base, dev + 10580);
    if ((mc & 7) != 5) {
      mc = (mc & ~7u) | 5;
      r20 |= 0x80008;
      S32(base, dev + 10580, mc);
      if (U8(base, dev + 10943) & 8) {
        r20 |= 1ull << 56;
        S32(base, dev + 10372, U32(base, dev + 10372) & ~0x000F0000u);
      }
    }
    r27 = (U32(base, r25 + 20) & 0x1F) != 0;
  } else {
    const uint32_t info = U32(base, ps + 64) + ps;
    r28 = info + 40;
    r18 = U32(base, info + 48);
    r17 = U32(base, info + 52);
    const uint32_t t = (r18 & 0x70000000u) > 0x10000000u ? 1 : 0;
    const uint8_t b42 = U8(base, dev + 10942), b43 = U8(base, dev + 10943);
    S8(base, dev + 10943, uint8_t(b43 | t));
    S8(base, dev + 10942, uint8_t(b42 | (t << 3)));
    if (r20 & 0x100000) {
      LiteralConstants(d, ps + 40, U32(base, ps + 24));
      r20 |= 0x4000;
      const uint32_t address = Phys(U32(base, info + 40) + U32(base, ps + 24));
      const uint32_t dwords = U32(base, info + 44) >> 2;
      d.out.Shader(1, address, dwords, 0);
      g_counts.shader_loads.fetch_add(1, std::memory_order_relaxed);
      uint32_t mc = U32(base, dev + 10580);
      S32(base, dev + 10536, U32(base, r28 + 24));
      if ((mc & 7) != 4) {
        mc = (mc & ~7u) | 4;
        r20 |= 0x80008;
        S32(base, dev + 10580, mc);
        if (U8(base, dev + 10943) & 8) {
          r20 |= 1ull << 56;
          S32(base, dev + 10372, U32(base, U32(base, dev + 12440) + 28));
        }
      }
    }
    r27 = ((U32(base, r28 + 20) ^ U32(base, r25 + 20)) & 0x7FFFF) != 0;
  }
  // loc_8222C1D0
  const uint32_t slot = ProgramSlot();
  if (!slot) return r20;
  S32(base, slot, U32(base, r25 + 8));
  const uint32_t r26 = U32(base, dev + 10580);
  const uint32_t r23 = U32(base, r25 + 12);
  uint32_t r29 = r26;
  if (U8(base, dev + 10940) & 0x40) {
    bool keep = false;
    if (r28) {
      const uint32_t f = U32(base, r28 + 28);
      keep = (U32(base, dev + 10556) & 8) || (f & 0x10) || (f & 0x20);
    }
    if (!keep) r29 = (r29 & ~7u) | 5;
    if (r29 != U32(base, dev + 12700) || (r20 & 8)) {
      r20 |= 8;
      S32(base, dev + 12700, r29);
    }
  }
  // loc_8222C26C
  const uint8_t b42 = U8(base, dev + 10942);
  if ((b42 & 0x80) || r27) {
    if (r20 & 0x180000) {
      S8(base, dev + 10942, b42 & 0xBF);
      LiteralConstants(d, vs + 872, U32(base, vs + 32));
      PatchAndLoad(d, r27, vs, slot, decl, r25, r28, r24);
    }
  } else if (r20 & 0x80000) {
    const uint32_t vinfo = vs + 872;
    LiteralConstants(d, vinfo, U32(base, vs + 32));
    S8(base, dev + 10942, U8(base, dev + 10942) & 0xBF);
    bool second = false;
    if (decl && !(U32(base, vinfo) & 0x40)) {
      if (U8(base, dev + 10940) & 0x80) {
        PatchAndLoad(d, 0, vs, slot, decl, r25, r28, r24);
      } else {
        if (BindingMatches(d, vs, decl, r24)) {
          r19 = int32_t(r24);
        } else {
          PatchAndLoad(d, 0, vs, slot, decl, r25, r28, r24);
        }
        second = ((r29 ^ r26) & 7) && (U32(base, vinfo) & 0x20) && BindingMatches(d, vs, decl, 1);
      }
    } else {
      r19 = int32_t(r24);
      second = ((r29 ^ r26) & 7) && (U32(base, vinfo) & 0x20);
    }
    if (second) {
      S8(base, dev + 10942, U8(base, dev + 10942) | 0x40);
      r15 = 1;
    }
  }
  // loc_8222C3CC
  if (r20 & 0x180000) {
    S32(base, dev + 10532, r23 | r17);
    r20 |= 0x18000;
    S32(base, dev + 10528, U32(base, slot) | r18);
  }
  auto load_binding = [&](int32_t binding) {
    const uint32_t info = U32(base, vs + (uint32_t(binding) + 112) * 8) + vs;
    const uint32_t address = Phys(U32(base, info + 872) + U32(base, vs + 32));
    const uint32_t dwords = U32(base, info + 876) >> 2;
    d.out.Shader(0, address, dwords, 0);
    S32(base, vs + uint32_t(binding) * 416 + 64, U32(base, dev + 10908));
    g_counts.shader_loads.fetch_add(1, std::memory_order_relaxed);
  };
  if (U8(base, dev + 10940) & 0x40) {
    // Predicated tiling: the band variants once, unpredicated.
    g_counts.predicated_tiling.fetch_add(1, std::memory_order_relaxed);
    if (r20 & 8) {
      d.out.Reg(0x2208, U32(base, dev + 10580));
      r20 &= ~uint64_t(8);
    }
    if (r20 & (1ull << 56)) {
      d.out.Reg(0x2001, U32(base, dev + 10372));
      r20 &= ~(1ull << 56);
    }
    if (r20 & 0x10000) {
      d.out.RegsFromGuest(base, 0x2180, dev + 10528, 2);
      r20 &= ~uint64_t(0x18000);
    }
    if (r19 != -1) {
      load_binding(r19);
    } else if (r15 != -1) {
      load_binding(r15);
    }
  } else if (r19 != -1) {
    load_binding(r19);
  }
  return r20;
}

void Flush(Device& d, const uint64_t (&masks)[5]) {
  uint8_t* base = d.base;
  const uint32_t dev = d.dev;
  g_counts.flushes.fetch_add(1, std::memory_order_relaxed);
  const uint64_t d0 = masks[0], d1 = masks[1], d3 = masks[3], d4 = masks[4];
  uint64_t d2 = masks[2];
  if (d0) {
    AluConstants(d, d0, 0x4000, dev + kVsConstants);
    S64(base, dev + 0, 0);
  }
  if (d1) {
    AluConstants(d, d1, 0x4400, dev + kPsConstants);
    S64(base, dev + 8, 0);
  }
  if (d2) {
    if (d2 & 0x1E0000) d2 = PendingShaders(d, d2);
    if (U64(base, dev + kPredicatedMask) & d2) {
      // SetPending_Predicated (sub_8226AAB8): predicated tiling's band
      // windows. One pass on the host; counted to see it never comes.
      g_counts.set_pending_predicated.fetch_add(1, std::memory_order_relaxed);
    }
    if (d2 & 0xFFF) RenderStates(d, d2 << 52, 0x2200, dev + kRegs2200);
    if (d2 & 0x1F000) RenderStates(d, (d2 << 47) & 0xF800000000000000ull, 0x2180, dev + kRegs2180);
    if (d2 & (0xFFFFull << 42)) {
      const uint64_t mask = (d2 << 6) & 0xFFFF000000000000ull;
      RenderStates(d, mask, 0x2000, dev + kRegs2000);
      Surfaces(d, mask);
    }
    if (d2 & 0x000003FFFFE00000ull) {
      RenderStates(d, (d2 << 22) & 0xFFFFF80000000000ull, 0x2100, dev + kRegs2100);
    }
    S64(base, dev + 16, 0);
  }
  if (d3) {
    if (d3 & 0xFFFFFFFFull) FetchConstants(d, d3 << 32);
    if (d3 & 0x007FFFFC00000000ull) {
      RenderStates(d, (d3 << 9) & 0xFFFFF80000000000ull, 0x2280, dev + kRegs2280);
    }
    S64(base, dev + 24, 0);
  }
  if (d4) {
    if (d4 & 0x3FFFFFFFFFull) RenderStates(d, (d4 << 26) & 0xFFFFFFFFFC000000ull, 0x2300, dev + kRegs2300);
    if (d4 & (0xFFull << 38)) RenderStates(d, (d4 << 18) & 0xFF00000000000000ull, 0x2380, dev + kRegs2380);
    if (d4 & (0x3Full << 49)) ClipPlanes(d, (d4 << 9) & 0xFC00000000000000ull);
    if (d4 & (1ull << 56)) RenderStates(d, 0xFFFFFFFFFF000000ull, 0x4900, dev + kBoolLoop);
    S64(base, dev + 32, 0);
  }
}

void FlushRenderStates(Device& d) {
  uint8_t* base = d.base;
  const uint32_t dev = d.dev;
  uint64_t d2 = U64(base, dev + 16);
  if (d2) {
    if (U64(base, dev + kPredicatedMask) & d2) g_counts.set_pending_predicated.fetch_add(1, std::memory_order_relaxed);
    if (d2 & 0xFFF) {
      RenderStates(d, d2 << 52, 0x2200, dev + kRegs2200);
      d2 &= ~0xFFFull;
    }
    if (d2 & 0x1F000) {
      RenderStates(d, (d2 << 47) & 0xF800000000000000ull, 0x2180, dev + kRegs2180);
      d2 &= ~0x1F000ull;
    }
    if (d2 & (0xFFFFull << 42)) {
      const uint64_t mask = (d2 << 6) & 0xFFFF000000000000ull;
      RenderStates(d, mask, 0x2000, dev + kRegs2000);
      Surfaces(d, mask);
      d2 &= ~(0xFFFFull << 42);
    }
    if (d2 & 0x000003FFFFE00000ull) {
      RenderStates(d, (d2 << 22) & 0xFFFFF80000000000ull, 0x2100, dev + kRegs2100);
      d2 &= ~0x000003FFFFE00000ull;
    }
    S64(base, dev + 16, d2);
  }
  const uint64_t d3 = U64(base, dev + 24);
  if (d3 & 0x007FFFFC00000000ull) {
    RenderStates(d, (d3 << 9) & 0xFFFFF80000000000ull, 0x2280, dev + kRegs2280);
    S64(base, dev + 24, d3 & ~0x007FFFFC00000000ull);
  }
  const uint64_t d4 = U64(base, dev + 32);
  if (d4 & 0x3FFFFFFFFFull) {
    RenderStates(d, (d4 << 26) & 0xFFFFFFFFFC000000ull, 0x2300, dev + kRegs2300);
    S64(base, dev + 32, d4 & ~0x3FFFFFFFFFull);
  }
}

void FlushAll(Device& d) {
  const uint64_t masks[5] = {U64(d.base, d.dev + 0), U64(d.base, d.dev + 8), U64(d.base, d.dev + 16),
                             U64(d.base, d.dev + 24), U64(d.base, d.dev + 32)};
  Flush(d, masks);
}

uint32_t VizToken(const uint8_t* base, uint32_t dev) {
  const uint32_t query = U8(base, dev + 12187);
  return (U8(base, dev + 12122 + query) & 0x3F) | (query ? 0x100 : 0);
}

uint32_t SplitCount(const uint8_t* base, uint32_t primitive, uint32_t count) {
  if (count <= 65535) return count;
  const uint32_t divisor = U32(base, kPrimitiveSplit + primitive * 8);
  if (!divisor) return count;
  return ((65535 / divisor) & ~1u) * divisor;
}

}  // namespace renut::native_d3d
