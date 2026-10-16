// Draws, clears and resolves.
#include "internal.h"

namespace renut::native_d3d {

bool DrawIndexedVertices(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32, primitive = ctx.r4.u32, base_vertex = ctx.r5.u32;
  const uint32_t first = ctx.r6.u32, total = ctx.r7.u32;
  const uint32_t serial = g_draw_serial.fetch_add(1, std::memory_order_relaxed) + 1;
  auto lock = LockUnlessOwn(dev);
  Capture(ctx, base, dev, [&](Device& d) {
    FlushAll(d);
    const uint32_t ib = U32(base, dev + kIndexBuffer);
    Indices(d, ib);
    const uint32_t ib_flags = U32(base, ib + 0), ib_base = U32(base, ib + 24);
    const bool wide = (ib_flags & 0x80000000u) != 0;
    const uint32_t endian = (ib_flags << 1) & 0xC0000000u;
    const uint32_t viz = VizToken(base, dev);
    const uint32_t extent = (U8(base, dev + 10940) & 1) ? kDrawScreenExtent : 0;
    uint32_t start = first, count = total;
    while (true) {
      const uint32_t chunk = SplitCount(base, primitive, count);
      d.out.Reg(0x2102, base_vertex);
      uint32_t initiator = (chunk << 16) | (primitive & 0x3F);
      uint32_t address, size;
      if (wide) {
        initiator |= 0x800;
        address = Phys(start * 4 + ib_base);
        size = ((chunk * 2) & 0x00FFFFFE) | endian;
      } else {
        address = Phys(start * 2 + ib_base);
        size = (chunk & 0x00FFFFFF) | endian;
      }
      if (extent) g_counts.screen_extent.fetch_add(1, std::memory_order_relaxed);
      d.out.Draw(kDrawPredicated | extent, viz, initiator, address, size, serial);
      g_counts.draws.fetch_add(1, std::memory_order_relaxed);
      const uint32_t left = count - chunk;
      if (!left) break;
      const uint32_t overlap = U32(base, kPrimitiveSplit + primitive * 8 + 4);
      start += chunk - overlap;
      count = left + overlap;
    }
  });
  return true;
}

bool DrawIndexedTessellatedVertices(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32, primitive = ctx.r4.u32, base_vertex = ctx.r5.u32;
  const uint32_t first = ctx.r6.u32, total = ctx.r7.u32;
  const uint32_t serial = g_draw_serial.fetch_add(1, std::memory_order_relaxed) + 1;
  auto lock = LockUnlessOwn(dev);
  Capture(ctx, base, dev, [&](Device& d) {
    const uint32_t ib = U32(base, dev + kIndexBuffer);
    S32(base, dev + 10612, (U32(base, dev + 10612) & ~3u) | 1);
    S64(base, dev + 24, U64(base, dev + 24) | (1ull << 50));
    uint32_t corners = 0;
    const bool adaptive = (U32(base, dev + 10616) & 3) == 2;
    if (adaptive) {
      const uint32_t row = 0x82002504 + (primitive - 16) * 12;
      corners = U32(base, row + 8);
      S32(base, dev + 10632, U32(base, row));
      S32(base, dev + 10636, U32(base, row + 4));
      S32(base, dev + 10640, U32(base, row + 8));
      const uint32_t pair = 0x82004140 + (primitive - 16) * 8;
      S32(base, dev + 10644, U32(base, pair));
      S32(base, dev + 10648, U32(base, pair + 4));
      S32(base, dev + 10652, 0x61412107);
      S32(base, dev + 10656, 0x66462606);
    } else {
      const uint32_t row = 0x82183888 + primitive * 12;
      S32(base, dev + 10632, U32(base, row));
      S32(base, dev + 10636, U32(base, row + 4));
      S32(base, dev + 10640, U32(base, row + 8));
      const bool wide = (U32(base, ib) & 0x80000000u) != 0;
      const uint32_t pair = (wide ? 0x82183A08 : 0x82183970) + primitive * 8;
      const uint32_t order = wide ? 0x61412101 : 0x30201000;
      S32(base, dev + 10644, U32(base, pair));
      S32(base, dev + 10648, U32(base, pair + 4));
      S32(base, dev + 10656, order);
      S32(base, dev + 10652, order);
    }
    S64(base, dev + 24, U64(base, dev + 24) | (0x7Full << 39));
    FlushAll(d);
    Indices(d, ib);
    const uint32_t ib_flags = U32(base, ib + 0), ib_base = U32(base, ib + 24);
    const uint32_t endian = ((ib_flags >> 29) & 3) << 30;
    const uint32_t viz = VizToken(base, dev);
    const uint32_t extent = (U8(base, dev + 10940) & 1) ? kDrawScreenExtent : 0;
    uint32_t start = first, count = total;
    while (true) {
      d.out.Reg(0x2102, base_vertex);
      uint32_t chunk = count;
      if (count > 65535) {
        const uint32_t divisor = U32(base, kTessSplit + primitive * 8);
        if (divisor) chunk = ((65535 / divisor) & ~1u) * divisor;
      }
      uint32_t initiator, address, size;
      if (adaptive) {
        const uint32_t indices = chunk * corners;
        initiator = (indices << 16) | (primitive & 0x3F) | 0x900;
        address = Phys(start * 4 + ib_base);
        size = ((indices << 1) & 0x00FFFFFE) | endian;
      } else if (ib_flags & 0x80000000u) {
        initiator = (chunk << 16) | (primitive & 0x3F) | 0x100 | 0x800;
        address = Phys(start * 4 + ib_base);
        size = ((chunk << 1) & 0x00FFFFFE) | endian;
      } else {
        initiator = (chunk << 16) | (primitive & 0x3F) | 0x100;
        address = Phys(start * 2 + ib_base);
        size = (chunk & 0x00FFFFFF) | endian;
      }
      if (extent) g_counts.screen_extent.fetch_add(1, std::memory_order_relaxed);
      d.out.Draw(kDrawPredicated | extent, viz, initiator, address, size, serial);
      g_counts.draws.fetch_add(1, std::memory_order_relaxed);
      const uint32_t left = count - chunk;
      if (!left) break;
      const uint32_t overlap = U32(base, kTessSplit + primitive * 8 + 4);
      start += chunk - overlap;
      count = left + overlap;
    }
  });
  return true;
}

bool DrawVertices(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32, primitive = ctx.r4.u32, first = ctx.r5.u32, total = ctx.r6.u32;
  const uint32_t serial = g_draw_serial.fetch_add(1, std::memory_order_relaxed) + 1;
  auto lock = LockUnlessOwn(dev);
  Capture(ctx, base, dev, [&](Device& d) {
    FlushAll(d);
    const uint32_t viz = VizToken(base, dev);
    const uint32_t extent = (U8(base, dev + 10940) & 1) ? kDrawScreenExtent : 0;
    uint32_t start = first, count = total;
    while (true) {
      const uint32_t chunk = SplitCount(base, primitive, count);
      d.out.Reg(0x2102, start);
      if (extent) g_counts.screen_extent.fetch_add(1, std::memory_order_relaxed);
      d.out.Draw(kDrawPredicated | extent, viz, (chunk << 16) | (primitive & 0x3F) | 0x80, 0, 0, serial);
      g_counts.draws.fetch_add(1, std::memory_order_relaxed);
      const uint32_t left = count - chunk;
      if (!left) break;
      const uint32_t overlap = U32(base, kPrimitiveSplit + primitive * 8 + 4);
      start += chunk - overlap;
      count = left + overlap;
    }
  });
  return true;
}

bool BeginVertices(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32, primitive = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  const uint32_t serial = g_draw_serial.fetch_add(1, std::memory_order_relaxed) + 1;
  auto lock = LockUnlessOwn(dev);
  g_counts.begin_vertices.fetch_add(1, std::memory_order_relaxed);
  const uint32_t dwords = (count * stride) >> 2;
  const uint32_t data = ScratchAlloc(dwords * 4);
  if (!data) {
    ctx.r3.u64 = 0;
    return true;
  }
  Capture(ctx, base, dev, [&](Device& d) {
    const uint32_t stride_dwords = (stride >> 2) & 0xFF;
    const uint8_t old_stride = U8(base, dev + 12528);
    S8(base, dev + 12528, uint8_t(stride_dwords));
    if (stride_dwords != U8(base, dev + 11832) && U32(base, dev + kVertexShader)) {
      S64(base, dev + 16, U64(base, dev + 16) | 0x80000);
    }
    S64(base, dev + 24, U64(base, dev + 24) & ~1ull);
    FlushAll(d);
    S8(base, dev + 12528, old_stride);
    d.out.Reg(0x2102, 0);
    const uint32_t fetch[6] = {1, 0, 1, 0, Phys(data) | 3, 0x10000002u | ((dwords << 2) & 0x03FFFFFCu)};
    d.out.Regs(0x4800 + 31 * 6, fetch, 6);
    d.out.Draw(kDrawPredicated, VizToken(base, dev), (count << 16) | (primitive & 0x3F) | 0x80, 0, 0, serial);
    g_counts.draws.fetch_add(1, std::memory_order_relaxed);
  });
  S64(base, dev + 24, U64(base, dev + 24) | 1);
  S32(base, dev + 13444, U32(base, dev + kRingWrite));
  S32(base, dev + 13448, data);
  S32(base, dev + 13456, dwords);
  ctx.r3.u64 = data;
  return true;
}

bool BeginIndexedVertices(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32, primitive = ctx.r4.u32, base_vertex = ctx.r5.u32;
  const uint32_t vertices = ctx.r6.u32, indices = ctx.r7.u32, format = ctx.r8.u32, stride = ctx.r9.u32;
  const uint32_t index_out = ctx.r10.u32, vertex_out = U32(base, ctx.r1.u32 + 84);
  const bool wide = format & 4;
  const uint32_t vertex_dwords = (vertices * stride) >> 2;
  const uint32_t index_dwords = wide ? indices : (indices + 1) >> 1;
  const uint32_t serial = g_draw_serial.fetch_add(1, std::memory_order_relaxed) + 1;
  auto lock = LockUnlessOwn(dev);
  g_counts.indexed_up.fetch_add(1, std::memory_order_relaxed);
  const uint32_t vertex_data = ScratchAlloc(vertex_dwords * 4), index_data = ScratchAlloc(index_dwords * 4);
  if (!vertex_data || !index_data) {
    if (index_out) S32(base, index_out, 0);
    if (vertex_out) S32(base, vertex_out, 0);
    ctx.r3.u64 = 0x8007000Eu;
    return true;
  }
  Capture(ctx, base, dev, [&](Device& d) {
    const uint8_t old_stride = U8(base, dev + 12528);
    const uint32_t stride_dwords = (stride >> 2) & 0xFF;
    S8(base, dev + 12528, uint8_t(stride_dwords));
    if (stride_dwords != U8(base, dev + 11832) && U32(base, dev + kVertexShader)) {
      S64(base, dev + 16, U64(base, dev + 16) | 0x80000);
    }
    FlushAll(d);
    S8(base, dev + 12528, old_stride);
    d.out.Reg(0x2102, base_vertex);
    const uint32_t fetch[6] = {0, 0, 0, 0, Phys(vertex_data) | 3,
                               0x10000002u | ((vertex_dwords << 2) & 0x03FFFFFCu)};
    d.out.Regs(0x4800 + 31 * 6, fetch, 6);
    const uint32_t endian = (format << 30) & 0xC0000000u;
    const uint32_t initiator = (indices << 16) | (primitive & 0x3F) | (wide ? 0x800 : 0);
    const uint32_t size = (wide ? ((indices << 1) & 0x00FFFFFEu) : (indices & 0x00FFFFFFu)) | endian;
    List& list = d.out.list();
    list.indices_named = false;  // the next indexed draw names its buffer again
    d.out.Draw(kDrawPredicated, VizToken(base, dev), initiator, Phys(index_data), size, serial);
    g_counts.draws.fetch_add(1, std::memory_order_relaxed);
  });
  S64(base, dev + 24, U64(base, dev + 24) | 1);
  if (index_out) S32(base, index_out, index_data);
  if (vertex_out) S32(base, vertex_out, vertex_data);
  S32(base, dev + 13444, U32(base, dev + kRingWrite));
  S32(base, dev + 13448, vertex_data);
  S32(base, dev + 13452, index_data);
  S32(base, dev + 13456, vertex_dwords);
  S32(base, dev + 13460, index_dwords);
  ctx.r3.u64 = 0;
  return true;
}

bool ClearNative(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32;
  uint32_t flags = ctx.r4.u32;
  const uint32_t rect = ctx.r5.u32, colour = ctx.r6.u32 ? ctx.r6.u32 : 0x82001160u;
  const float z = float(ctx.f1.f64);
  const uint32_t stencil = ctx.r8.u32;
  std::lock_guard lock(g_front.mutex);
  if (!U32(base, dev + 12456)) flags &= ~0xF0u;
  if (!flags) return true;
  auto viewport = [&](uint32_t at) {
    float f;
    const uint32_t bits = U32(base, dev + at);
    std::memcpy(&f, &bits, 4);
    return int32_t(f);
  };
  const int32_t vx = viewport(12648), vy = viewport(12652), vw = viewport(12656), vh = viewport(12660);
  int32_t x1 = std::max(int32_t(U32(base, rect + 0)), vx), y1 = std::max(int32_t(U32(base, rect + 4)), vy);
  int32_t x2 = std::min(int32_t(U32(base, rect + 8)), vx + vw), y2 = std::min(int32_t(U32(base, rect + 12)), vy + vh);
  if (U32(base, dev + 11856)) {
    x1 = std::max(x1, int32_t(U32(base, dev + 12676)));
    y1 = std::max(y1, int32_t(U32(base, dev + 12680)));
    x2 = std::min(x2, int32_t(U32(base, dev + 12684)));
    y2 = std::min(y2, int32_t(U32(base, dev + 12688)));
  }
  if (x2 <= x1 || y2 <= y1) return true;
  Capture(ctx, base, dev, [&](Device& d) {
    FlushRenderStates(d);
    uint32_t present = U32(base, dev + 12456) ? 0x10 : 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (U32(base, dev + 12440 + i * 4)) present |= 1u << i;
    }
    List& list = d.out.list();
    list.words.push_back(kOpClear | (flags << 8));
    for (int32_t v : {x1, y1, x2, y2}) list.words.push_back(uint32_t(v));
    for (uint32_t i = 0; i < 4; ++i) list.words.push_back(U32(base, colour + i * 4));
    uint32_t z_bits;
    std::memcpy(&z_bits, &z, 4);
    list.words.push_back(z_bits);
    list.words.push_back(stencil);
    // RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO, RB_COLOR1..3_INFO.
    const uint32_t r = dev + kRegs2000;
    for (uint32_t index : {0u, 1u, 3u, 4u, 5u, 2u}) list.words.push_back(U32(base, r + index * 4));
    list.words.push_back(present);
  });
  S8(base, dev + 10941, U8(base, dev + 10941) | 4);
  S64(base, dev + 16, U64(base, dev + 16) | 0x20000);
  return true;
}

uint32_t ResolveScratch() {
  static uint32_t block = 0;
  if (!block) {
    if (auto* memory = rex::system::kernel_memory()) block = memory->SystemHeapAlloc(256, 16);
  }
  return block;
}

bool ResolveNative(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32;
  uint32_t flags = ctx.r4.u32;
  uint32_t src_rect = ctx.r5.u32;
  const uint32_t texture = ctx.r6.u32;
  uint32_t dest_point = ctx.r7.u32;
  const uint32_t level = ctx.r8.u32, slice = ctx.r9.u32;
  uint32_t clear_color = ctx.r10.u32;
  const double clear_z = ctx.f1.f64;
  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t clear_stencil = U32(base, entry_sp + 92), params = U32(base, entry_sp + 100);
  const uint32_t scratch = ResolveScratch();
  if (!scratch || !texture) return false;
  std::lock_guard lock(g_front.mutex);
  PPCContext saved = ctx;
  const uint32_t source = flags & 7;
  if (!(flags & 0x70)) {
    const uint32_t surface = source == 4 ? U32(base, dev + 12456) : U32(base, dev + 12440 + source * 4);
    if (surface) {
      const uint32_t kind = U16(base, surface + 24) & 3;
      flags |= kind == 0 ? 16 : kind == 1 ? 80 : 112;
    }
  }
  if (!clear_color) clear_color = 0x82001160;
  // The destination's fetch constant (header +28..+48).
  const uint32_t d0 = U32(base, texture + 28), d1 = U32(base, texture + 32), d2 = U32(base, texture + 36);
  const uint32_t d3 = U32(base, texture + 40), d5 = U32(base, texture + 48);
  const uint32_t packed = (__builtin_rotateleft32(d3, 2) & 2) + 1;
  const uint32_t dimension = (d5 >> 9) & 3;
  uint32_t width, height, depth_step;
  if (dimension != 2) {
    width = (d2 & 0x1FFF) + packed;
    height = ((d2 >> 13) & 0x1FFF) + packed;
    depth_step = 1;
  } else {
    width = (d2 & 0x7FF) + packed;
    height = ((d2 >> 11) & 0x7FF) + packed;
    depth_step = 8;
  }
  uint32_t address = d1 & 0xFFFFF000u;
  const uint32_t format = d1 & 0x3F;
  uint32_t pitch = __builtin_rotateleft32(d0, 15) & 0x3FE0;
  const uint32_t block_bytes = (U16(base, 0x820052F8 + format * 2) >> 3) & 0x1F;
  // Level or slice: Direct3D's own lookup, outputs in scratch.
  const uint32_t o_pitch = scratch + 0, o_height = scratch + 4, o_width = scratch + 8, o_address = scratch + 12;
  if (level || slice) {
    S32(base, o_height, height);
    S32(base, o_width, width);
    S32(base, o_address, address);
    const uint32_t frame = (entry_sp - 0x400) & ~15u;
    S32(base, frame + 84, o_height);
    S32(base, frame + 92, scratch + 16);
    S32(base, frame + 100, o_address);
    ctx.r1.u64 = frame;
    ctx.r3.u64 = texture;
    ctx.r4.u64 = slice & ~(depth_step - 1);
    ctx.r5.u64 = level;
    ctx.r6.u64 = o_pitch;
    ctx.r7.u64 = scratch + 20;
    ctx.r8.u64 = scratch + 24;
    ctx.r9.u64 = scratch + 28;
    ctx.r10.u64 = o_width;
    sub_82207680(ctx, base);
    ctx = saved;
    height = U32(base, o_height);
    width = U32(base, o_width);
    address = U32(base, o_address);
    if (block_bytes) pitch = U32(base, o_pitch) / block_bytes;
  }
  // The source rectangle (the whole destination level by default) and the
  // destination point.
  int32_t rx1, ry1, rx2, ry2;
  if (src_rect) {
    rx1 = int32_t(U32(base, src_rect)), ry1 = int32_t(U32(base, src_rect + 4));
    rx2 = int32_t(U32(base, src_rect + 8)), ry2 = int32_t(U32(base, src_rect + 12));
  } else {
    rx1 = 0, ry1 = 0, rx2 = int32_t(width), ry2 = int32_t(height);
  }
  if (!dest_point) dest_point = 0x82000EE0;
  const int32_t px = int32_t(U32(base, dest_point)), py = int32_t(U32(base, dest_point + 4));
  const uint32_t physical = address & 0x1FFFFFFF;
  const uint32_t high = ((address >> 20) + 512) & 0x1000;
  uint32_t dest_base = uint32_t((py - ry1) * int32_t(pitch) + ((px - rx1) << 5)) * block_bytes + physical + high;
  const uint32_t dest_height = uint32_t(ry1 - py) + height;
  // RB_COPY_DEST_INFO: format (some remapped), the number type, endian.
  uint32_t dest_format = format;
  switch (format) {
    case 54: dest_format = 7; break;
    case 55: dest_format = 16; break;
    case 56: dest_format = 17; break;
    case 27: dest_format = 30; break;
    case 28: dest_format = 31; break;
    case 29: dest_format = 32; break;
    case 22: case 23: dest_format = 6; break;
    default: break;
  }
  const uint32_t sign = (d0 >> 2) & 3;
  uint32_t number = sign == 1 ? 1 : 0;
  if (d3 & 1) number += 2;
  switch (dest_format) {
    case 30: case 31: case 32: case 36: case 37: case 38: case 57: case 63: number = 7; break;
    default: break;
  }
  const uint32_t not_e = (d3 & 0xE) ? 1 : 0;
  uint32_t bias = ((flags >> 26) & 0x3F);
  bias = ((bias & ~0x100u) | ((not_e << 8) & 0x100)) & 0x13F;
  number = (number & 7) | (bias << 3);
  uint32_t info = (dest_format & 0x3F) | (number << 6);
  info = ((info << 7) & 0xFFFFFF80u) | ((d1 >> 6) & 3);
  const uint32_t dest_pitch = ((dest_height << 16) & 0x3FFF0000u) | (pitch & 0x3FFF);
  if (dimension == 2) {
    S32(base, dev + 10820, pitch * height);
    S64(base, dev + 32, U64(base, dev + 32) | 4);
    info |= (((depth_step - 1) & slice) << 4) & 0x70;
    info |= 8;
  }
  uint32_t samples = 1;
  if (flags & 0x2000) {
    samples = 3;
  } else if (source == 4) {
    samples = 0;
  }
  const uint32_t control = ((samples << 20) & 0x300000) | ((flags & 0x377) - 16);
  S32(base, dev + 10788, info);
  S32(base, dev + 10784, dest_pitch);
  S32(base, dev + 10780, dest_base);
  S32(base, dev + 10776, control);
  S64(base, dev + 32, U64(base, dev + 32) | 0x3C00);
  if (flags & 0x200) {
    ctx.r3.u64 = dev;
    ctx.r4.u64 = params;
    ctx.f1.f64 = clear_z;
    ctx.r6.u64 = clear_stencil;
    sub_821FA520(ctx, base);
    ctx = saved;
  }
  if (flags & 0x100) {
    ctx.r3.u64 = dev;
    ctx.r4.u64 = params;
    ctx.r5.u64 = control & 7;
    for (uint32_t i = 0; i < 16; ++i) ctx.v1.u8[i] = GuestPtr(base, clear_color)[15 - i];  // as lvx128 loads it
    sub_82290DE0(ctx, base);
    ctx = saved;
  }
  // The rectangle the copy draw would cover, resolve-aligned.
  const int32_t x0 = rx1 & ~7, y0 = ry1 & ~7, x1 = (rx2 + 7) & ~7, y1 = (ry2 + 7) & ~7;
  Capture(ctx, base, dev, [&](Device& d) {
    FlushRenderStates(d);
    List& list = d.out.list();
    list.words.push_back(kOpResolve);
    for (int32_t v : {x0, y0, x1, y1}) list.words.push_back(uint32_t(v));
    // RB_COPY_CONTROL .. RB_COPY_SURFACE_SLICE (0x2318-0x2323) and the
    // surface registers RB_SURFACE_INFO .. RB_COLOR3_INFO (0x2000-0x2005).
    for (uint32_t r = 0x2318; r <= 0x2323; ++r) list.words.push_back(U32(base, dev + kRegs2300 + (r - 0x2300) * 4));
    for (uint32_t r = 0; r < 6; ++r) list.words.push_back(U32(base, dev + kRegs2000 + r * 4));
    // Bit 8: the capture Photo Mode's routine is making (see NativeResolve).
    list.words.push_back(source | (renut_in_photo_capture() ? 0x100u : 0u));
    // The destination object: the level, slice or face the copy goes to, as
    // Direct3D's own lookup found it (3D slices interleave and stay by address).
    const bool to_object = dimension != 2;
    list.words.push_back(to_object ? Phys(address) : 0);
    list.words.push_back(width);
    list.words.push_back(height);
    list.words.push_back(uint32_t(px - rx1));
    list.words.push_back(uint32_t(py - ry1));
    list.words.push_back(pitch);
  });
  // What the copy leaves in the device: the texture's fence, and the state it
  // would have clobbered marked for the next draw (harmless on the host).
  S32(base, texture + 8, U32(base, dev + 10908));
  uint64_t d2m = U64(base, dev + 16);
  d2m |= (1ull << 41) | (1ull << 40) | (1ull << 39) | 0xE0 | (1ull << 44) | 0x80000 | 8 | 0x18000 | 0x800 | 0x100;
  S64(base, dev + 16, d2m);
  S64(base, dev + 24, U64(base, dev + 24) | (1ull << 35) | 0x80000000ull);
  S64(base, dev + 32, U64(base, dev + 32) | 0x80000);
  ctx = saved;
  return true;
}

}  // namespace renut::native_d3d
