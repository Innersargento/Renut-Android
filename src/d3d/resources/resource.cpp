// Layer 2: resources, natively. Ported from reNut's hooks/resource.cpp
// (masterspike52's reNut), which writes N&B's resource headers, texture layout,
// locks and teardown from the game's own disassembly. What it did for the PC
// renderer (host records, render-thread queues, texture invalidation) is left out:
// this app's backend reads the headers and their memory directly.
//
// Addresses and constants are this executable's (retail.h); reNut's are title
// update 3's. Checked against the executable with idalib on 2026-10-30:
// CreateVertexBuffer (0x82654DB8), CreateTexture (0x82655EC8) and SetSurfaceHeader
// (0x82655A50) write what the code below writes; the format table is 0x820052F8 and
// the HiZ owner word 0x82FA36C8.
//
// The recompiled game and kernel are reached through GuestCalls (hooks.cpp): the
// allocator's physical memory, XMemFree for what the game allocated itself, and the
// fence wait.
#include "d3d/resources/resource.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <mutex>
#include <unordered_set>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "d3d/device/getters.h"
#include "d3d/device/internal.h"
#include "d3d/device/retail.h"

namespace renut::d3d::port {

namespace {

// Big-endian words of guest memory, as reNut's code names them.
struct be_u32 {
  u32 raw;
  operator u32() const { return __builtin_bswap32(raw); }
  be_u32& operator=(u32 value) {
    raw = __builtin_bswap32(value);
    return *this;
  }
};
struct be_u16 {
  u16 raw;
  operator u16() const { return __builtin_bswap16(raw); }
};

// A guest pointer to words a call writes to.
class Mapped {
 public:
  explicit Mapped(be_u32* pointer) : pointer_(pointer) {}
  explicit operator bool() const { return pointer_ != nullptr; }
  be_u32& operator[](size_t index) const { return pointer_[index]; }
  be_u32& operator*() const { return *pointer_; }

 private:
  be_u32* pointer_;
};
using mapped_u32 = Mapped;

GuestCalls g_calls;

uint8_t* GuestBase() {
  static uint8_t* base = [] {
    auto* memory = rex::system::kernel_memory();
    return memory ? memory->virtual_membase() : nullptr;
  }();
  return base;
}

template <typename T>
T* Ptr(u32 va) {
  return va && GuestBase() ? reinterpret_cast<T*>(renut::GuestPtr(GuestBase(), va)) : nullptr;
}

u32 LoadU32(u32 va) {
  const auto* p = Ptr<const be_u32>(va);
  return p ? u32(*p) : 0;
}

void StoreU32(u32 va, u32 value) {
  if (auto* p = Ptr<be_u32>(va))
    *p = value;
}

// lwarx/stwcx. add on a guest word; returns the old value.
u32 AtomicAddGuest(u32 va, u32 delta) {
  auto* p = Ptr<u32>(va);
  if (!p)
    return 0;
  std::atomic_ref<u32> word(*p);
  u32 raw = word.load(std::memory_order_relaxed);
  for (;;) {
    const u32 old = __builtin_bswap32(raw);
    if (word.compare_exchange_weak(raw, __builtin_bswap32(old + delta), std::memory_order_acq_rel))
      return old;
  }
}

void WaitForFence(u32 fence) {
  if (g_calls.wait_fence)
    g_calls.wait_fence(fence);
}

constexpr u32 kAllocPhysical = 0xB1800000u;  // XMemFree attributes of resource data
constexpr u32 kAllocHeader = 0x24800000u;    // ... of resource headers
// Common bit the XDK sets on every resource it creates itself.
constexpr u32 kCommonD3DCreated = 0x100000;
// A surface or volume of a texture's level.
constexpr u32 kCommonTextureLevelBit = 0x40000000u;
// The word set while a depth surface owns the HiZ memory.
constexpr u32 kHiZOwnerVa = 0x82FA36C8;
// XPhysicalAlloc drops MEM_LARGE_PAGES while this word is set.
constexpr u32 kGameNoLargePagesVa = 0x82F39E34;

u32 Slw(u32 value, u32 amount) {  // slw: 6-bit amount, 32 and up gives 0
  amount &= 63;
  return amount >= 32 ? 0 : value << amount;
}

// ---------------------------------------------------------------------------
// Resource memory (what D3D's XMemAlloc calls get)
// ---------------------------------------------------------------------------

std::mutex g_owned_mutex;
std::unordered_set<u32> g_owned_headers;  // headers the native D3D allocated

// A zeroed header (XMemAlloc 0x64800000: heap, 16-byte aligned, zeroed).
u32 AllocHeader(u32 size) {
  auto* memory = rex::system::kernel_memory();
  if (!memory)
    return 0;
  const u32 va = memory->SystemHeapAlloc(size, 16);
  if (!va)
    return 0;
  std::memset(Ptr<u8>(va), 0, size);
  std::lock_guard lock(g_owned_mutex);
  g_owned_headers.insert(va);
  return va;
}

bool OwnsHeader(u32 va) {
  std::lock_guard lock(g_owned_mutex);
  return g_owned_headers.count(va) != 0;
}

void FreeHeader(u32 va) {
  {
    std::lock_guard lock(g_owned_mutex);
    if (!g_owned_headers.erase(va))
      return;
  }
  if (auto* memory = rex::system::kernel_memory())
    memory->SystemHeapFree(va);
}

// Resource data: XMemAlloc's physical path for its attributes (0x82265B90: the
// alignment from bits 24-27, the protection from bits 28-29), then XPhysicalAlloc
// (0x82716060), which asks the kernel. 0 on failure.
u32 AllocPhysical(u32 size, u32 attributes) {
  static constexpr u32 kAlignment[16] = {4,    4,    4,     8,     16,    32,     64,     128,
                                         256,  512,  0x400, 0x800, 0x1000, 0x2000, 0x4000, 0x8000};
  static constexpr u32 kProtect[4] = {0x20000404, 0x204, 4, 0x404};
  if (!size || !g_calls.alloc_physical)
    return 0;
  u32 protect = kProtect[(attributes >> 28) & 3];
  if (LoadU32(kGameNoLargePagesVa))
    protect &= ~0x20000000u;
  const u32 va = g_calls.alloc_physical(size, protect, kAlignment[(attributes >> 24) & 0xF]);
  if (!va)
    REXLOG_ERROR("D3D: {} bytes of physical memory not available", size);
  return va;
}

void FreePhysical(u32 va) {
  if (va && g_calls.free_physical)
    g_calls.free_physical(va);
}

// Memory the game's own allocator gave a resource goes back to it.
void GameXMemFree(u32 va, u32 attributes) {
  if (va && g_calls.xmem_free)
    g_calls.xmem_free(va, attributes);
}

u16 FormatTableEntry(u32 format) {
  const auto* table = Ptr<const be_u16>(retail::kFormatTable);
  return table ? u16(table[format & 0x3F]) : 0;
}

u32 FormatBitsPerPixel(u32 format) {
  return FormatTableEntry(format) & 0xFF;
}

u32 AddRef(u32 resource) {
  return AddRefResource(GuestOnly(GuestBase()), resource);
}

u32 ResourceTypeOf(u32 resource) {
  return ResourceType(GuestOnly(GuestBase()), resource);
}

// ---------------------------------------------------------------------------
// EDRAM (D3D's Edram allocator: sub_82661C38 and MarkBits 0x82661AF0)
// ---------------------------------------------------------------------------

// 0x1800 tiles of 5120 bytes, a bit each (most significant first); the word
// after the bitmap is a sentinel the searches stop at.
constexpr u32 kEdramTiles = 0x1800;
constexpr u32 kEdramWords = kEdramTiles / 32;

struct Edram {
  std::mutex mutex;
  u32 free_tiles = kEdramTiles;
  u32 bits[kEdramWords + 1] = {};
  Edram() { bits[kEdramWords] = 0x55555555u; }
};

Edram &EdramState() {
  static Edram edram;
  return edram;
}

u32 Shr(u32 value, u32 shift) {  // srw: 0 for shifts of 32 and up
  return shift >= 32 ? 0 : value >> shift;
}

void MarkBits(Edram &edram, u32 first, u32 end, bool set) {
  const u32 first_word = first >> 5, last_word = end >> 5;
  const u32 first_bit = first & 31, end_bit = end & 31;
  u32 *p = edram.bits + first_word;
  u32 *const last = edram.bits + last_word;
  if (set) {
    if (first_word == last_word) {
      *p |= Shr(~0u, first_bit) & ~Shr(~0u, end_bit);
      return;
    }
    *p++ |= Shr(~0u, first_bit);
    while (p < last)
      *p++ = ~0u;
    *p |= ~Shr(~0u, end_bit);
  } else {
    if (first_word == last_word) {
      *p &= ~(Shr(~0u, first_bit) & ~Shr(~0u, end_bit));
      return;
    }
    *p++ &= ~Shr(~0u, first_bit);
    while (p < last)
      *p++ = 0;
    *p &= Shr(~0u, end_bit);
  }
}

u32 CountLeadingZeros(u32 v) {
  return v ? u32(__builtin_clz(v)) : 32;
}

// The smallest free run of at least 'tiles', or -1.
i32 AllocateEdram(u32 tiles) {
  Edram &edram = EdramState();
  std::lock_guard lock(edram.mutex);
  if (tiles > edram.free_tiles)
    return -1;
  const u32 *const words = edram.bits;
  const u32 *const sentinel = edram.bits + kEdramWords;
  u32 cursor = 0, best_start = 0, best_length = ~0u;
  for (;;) {
    const u32 *w = words + (cursor >> 5);
    if (w == sentinel)
      break;
    u32 word = *w;
    u32 free_bit = CountLeadingZeros(Shr(~0u, cursor & 31) & ~word);
    if (free_bit == 32) {
      do {
        ++w;
        word = *w;
      } while (word == ~0u);
      if (w == sentinel)
        break;
      free_bit = CountLeadingZeros(~word);
    }
    const u32 *end_word = w;
    u32 end_bit = CountLeadingZeros(Shr(~0u, free_bit) & word);
    if (end_bit == 32) {
      ++end_word;
      while (*end_word == 0)
        ++end_word;
      end_bit = end_word == sentinel ? 0 : CountLeadingZeros(*end_word);
    }
    const u32 start = u32(w - words) * 32 + free_bit;
    const u32 length = u32(end_word - words) * 32 - start + end_bit;
    if (!length)
      break;
    if (length >= tiles && length < best_length) {
      best_start = start;
      best_length = length;
    }
    cursor = start + length;
  }
  if (best_length == ~0u)
    return -1;
  edram.free_tiles -= tiles;
  MarkBits(edram, best_start, best_start + tiles, true);
  return i32(best_start);
}

void FreeEdram(u32 base, u32 tiles) {
  // A range this allocator never handed out (a surface made before the native D3D took
  // over) must not credit tiles or clear someone else's bits.
  if (base >= kEdramTiles || tiles > kEdramTiles - base)
    return;
  Edram &edram = EdramState();
  std::lock_guard lock(edram.mutex);
  edram.free_tiles = std::min(edram.free_tiles + tiles, kEdramTiles);
  MarkBits(edram, base, base + tiles, false);
}


// The EDRAM tiles a surface takes (sub_8231A808).
u32 EdramTileCount(u32 width, u32 height, u32 format, u32 multisample) {
  const u32 f = format & 0x3F;
  u32 w = width, h = height;
  if (i32(multisample) >= 1)
    h = 2 * height;
  if (multisample == 2)
    w = 2 * width;
  const u32 units = (f == 21 || f == 32 || f == 37) ? 8 : 4;
  return ((h + 15) & ~15u) * 80 * ((w + 79) / 80) * units / 0x1400;
}

// D3D::SetSurfaceHeader(Width, Height, D3DFORMAT, D3DMULTISAMPLE_TYPE,
//     const D3DSURFACE_PARAMETERS*, D3DSurface*, DWORD *pEdramTiles,
//     DWORD *pHiZSize) [0x82655A50]
void SetSurfaceHeader(u32 width, u32 height, u32 format, u32 multisample, u32 parameters_va,
                      be_u32 *s, u32 &edram_tiles, u32 &hiz_size) {
  u32 f = format & 0x3F;
  const bool depth = f == 22 || f == 23;
  if (f == 54)
    f = 7;
  s[1] = 1u;            // ReferenceCount
  s[0] = 4u;            // Common: D3DRTYPE_SURFACE
  s[5] = 0xFFFF0000u;   // BaseFlush
  i32 hiz_base = -1;
  const u32 tiles = EdramTileCount(width, height, format, multisample);
  hiz_size = 0;
  if (depth) {
    u32 w = width, h = height;
    if (i32(multisample) >= 1)
      h = 2 * height;
    if (multisample == 2)
      w = 2 * width;
    hiz_size = (((w + 31) >> 5) * ((h + 15) >> 4)) & 0x7FFFFF;
  }
  const auto *parameters = Ptr<const be_u32>(parameters_va);
  u32 base = 0, color_exp_bias = 0;
  if (parameters) {
    base = parameters[0];
    hiz_base = i32(u32(parameters[1]));
    color_exp_bias = parameters[2];
  } else if (depth && !LoadU32(kHiZOwnerVa)) {
    hiz_base = 0;
  }
  u32 pitch_tiles, pitch;
  if (multisample == 0 || multisample == 1) {
    pitch_tiles = 80 * ((width + 79) / 80);
    pitch = (width + 31) & ~31u;
  } else {
    pitch_tiles = 40 * ((width + 39) / 40);
    pitch = (width + 15) & ~15u;
  }
  s[10] = format;  // +0x28 Format
  u32 size = ((width - 1) << 18) | (u32(s[9]) & 7);
  size = Insert(size, height - 1, 3, 15);
  s[9] = size;  // +0x24 Size
  s[6] = (((pitch << 2) | (multisample & 3)) << 16) | (pitch_tiles & 0x3FFF);  // +0x18 SurfaceInfo
  u32 format_flags = Extract(FormatTableEntry(f), 8, 4);
  if (depth) {
    s[7] = ((format_flags & 1) << 16) | (base & 0xFFF);  // +0x1C DepthInfo
    if (hiz_base == -1) {
      s[8] = 0u;  // +0x20 HiControl
    } else {
      u32 hiz_flag;
      if (parameters && u32(parameters[3]))
        hiz_flag = u32(parameters[3]) - 1;
      else
        hiz_flag = f != 22 ? 1 : 0;
      s[8] = (((hiz_flag & 1) | (u32(hiz_base) << 13)) << 4) | 1;
    }
  } else {
    if (!format_flags && !(format & 0x20000) && (format & 0x600) == 0x600 &&
        (format & 0x1800) == 0x1800 && (format & 0x6000) == 0x6000 && !(format & 0x18000))
      format_flags = 1;
    format_flags = Insert(format_flags, color_exp_bias, 4, 6);
    s[7] = ((format_flags & 0x3FF) << 16) | (base & 0xFFF);  // +0x1C ColorInfo
  }
  s[11] = tiles * 0x1400;  // +0x2C EDRAM size
  edram_tiles = tiles;
}


// D3DSurface *D3DDevice_CreateSurface(UINT Width, UINT Height, D3DFORMAT,
//     D3DMULTISAMPLE_TYPE, const D3DSURFACE_PARAMETERS *pParameters) [0x82655FE8]
// Without parameters the surface gets EDRAM (and the first depth surface HiZ).
u32 CreateSurfaceImpl(u32 width, u32 height, u32 format, u32 multisample, u32 parameters_va) {
  const u32 header = AllocHeader(0x30);
  if (!header)
    return 0;
  auto* s = Ptr<be_u32>(header);
  u32 tiles = 0, hiz_size = 0;
  SetSurfaceHeader(width, height, format, multisample, parameters_va, s, tiles, hiz_size);
  s[0] = u32(s[0]) | kCommonD3DCreated;
  if (!parameters_va) {
    const u32 f = u32(s[10]) & 0x3F;
    const bool depth = f == 22 || f == 23;
    const i32 base = AllocateEdram(tiles);
    if (base < 0) {
      FreeHeader(header);
      return 0;
    }
    if (u32(base) + tiles > 0x800) {
      FreeEdram(u32(base), tiles);
      FreeHeader(header);
      return 0;
    }
    s[0] = u32(s[0]) | 0x80000000u;  // owns its EDRAM
    s[7] = Insert(s[7], u32(base), 0, 12);
    if (depth && !LoadU32(kHiZOwnerVa)) {
      StoreU32(kHiZOwnerVa, 1);
      s[8] = u32(s[8]) & 0x1FFFF;
    }
  }
  return header;
}

// ---------------------------------------------------------------------------
// Subresource layout
// ---------------------------------------------------------------------------

// BlockSizeOfGpuFormat (0x82207B90).
void BlockSize(u32 format, u32 &width, u32 &height) {
  const u32 f = format;
  if (f <= 0x2C) {
    if (f == 0x2C) {
      width = 4, height = 1;
    } else if (f < 0xB) {
      width = height = 1;
    } else if (f <= 0xC) {
      width = 2, height = 1;
    } else if (f <= 0x11) {
      width = height = 1;
    } else if (f <= 0x14) {
      width = height = 4;
    } else if (f == 0x27) {
      width = 4, height = 1;
    } else if (f == 0x28) {
      width = 2, height = 1;
    } else {
      width = height = 1;
    }
  } else if (f == 0x31 || (f > 0x32 && f <= 0x35) || (f > 0x39 && f <= 0x3D)) {
    width = height = 4;
  } else {
    width = height = 1;
  }
}

// AlignTextureDimensions (0x82207C28).
u32 AlignTextureDimensions(u32 &width, u32 &height, u32 &depth, u32 bpp, u32 format,
                           u32 dimension, bool tiled) {
  u32 row_blocks = 32;
  const u32 column_blocks = dimension == 0 ? 1 : 32;
  const u32 depth_align = dimension == 2 ? 4 : 1;
  u32 block_width, block_height;
  BlockSize(format, block_width, block_height);
  if (!tiled) {
    const u32 block_bytes = (block_width * block_height * bpp) >> 3;
    if (block_bytes && 0x100 / block_bytes >= 32)
      row_blocks = 0x100 / block_bytes;
  }
  const u32 wa = block_width * row_blocks, ha = block_height * column_blocks;
  width = (width + wa - 1) & ~(wa - 1);
  height = (height + ha - 1) & ~(ha - 1);
  depth = (depth + depth_align - 1) & ~(depth_align - 1);
  const u32 plane = ((width * bpp) >> 3) * height;
  return dimension == 2 ? (depth * plane + 0xFFF) & ~0xFFFu : ((plane + 0xFFF) & ~0xFFFu) * depth;
}

u32 CeilLog2(u32 v) {
  return 32 - CountLeadingZeros(v - 1);
}

// GetMipTailLevelOffsetCoords (0x826555E0): bytes from the tail level to
// 'level' within it, and its block coordinates.
u32 MipTailOffset(u32 level, u32 width, u32 height, u32 depth, u32 row_pitch, u32 slice_pitch,
                  u32 format, u32 &x_blocks, u32 &y_blocks, u32 &z) {
  const u32 bpp = FormatBitsPerPixel(format);
  u32 block_width, block_height;
  BlockSize(format, block_width, block_height);
  const u32 log2_width = CeilLog2(width);
  const u32 log2_height = CeilLog2(height);
  const u32 log2_depth = CeilLog2(depth);
  const u32 size[2] = {1u << log2_width, 1u << log2_height};
  u32 coords[2] = {0, 0};
  z = 0;
  const u32 index = log2_height < log2_width ? 1 : 0;
  if (level < 3) {
    coords[index] = u32(16 >> level);
  } else {
    const u32 other = index ^ 1;
    coords[other] = Shr(size[other], level - 2);
    if (coords[other] < 4) {
      u32 slices = log2_depth - level;
      if (slices <= 1)
        slices = 1;
      z = 4 * slices;
    }
  }
  x_blocks = coords[0] / block_width;
  y_blocks = coords[1] / block_height;
  return ((block_width * bpp * coords[0]) >> 3) + coords[1] * row_pitch + z * slice_pitch;
}

// What sub_82207680 reports for one level and array slice of a texture.
struct Subresource {
  u32 address = 0;      // CPU address of the slice of the level
  u32 row_pitch = 0;    // bytes per row of blocks... of texels for the base
  u32 slice_bytes = 0;  // bytes per 2D slice
  u32 level_bytes = 0;  // bytes of the level (all its depth slices)
  u32 tail_offset = 0;  // bytes into the packed mip tail
  u32 width = 0, height = 0, depth = 0;
};

// D3D's subresource layout (sub_82207680), on a texture header.
Subresource SubresourceLayout(u32 texture, u32 slice, u32 level) {
  Subresource out;
  const auto *t = Ptr<const be_u32>(texture);
  if (!t)
    return out;
  const u32 dword0 = t[7], dword1 = t[8], dword2 = t[9], dword3 = t[10], dword4 = t[11],
            dword5 = t[12];
  const u32 border = dword3 >> 31;
  const bool tiled = (dword0 >> 31) != 0;
  const u32 dimension = Extract(dword5, 9, 2);
  const u32 bpp = FormatBitsPerPixel(dword1);
  const u32 format = dword1 & 0x3F;
  // GetTextureDimensions (0x82207AF8).
  const u32 add = ((dword3 >> 30) & 2) + 1;
  u32 width, height, depth = 1;
  if (dimension == 0) {
    width = (dword2 & 0xFFFFFF) + add;
    height = 1;
  } else if (dimension == 2) {
    width = (dword2 & 0x7FF) + add;
    height = ((dword2 >> 11) & 0x7FF) + add;
    depth = (dword2 >> 22) + add;
  } else {
    width = (dword2 & 0x1FFF) + add;
    height = ((dword2 >> 13) & 0x1FFF) + add;
  }
  const u32 b2 = border * 2;
  auto pow2 = [border](u32 v) {
    const u32 shift = border + 32 - CountLeadingZeros(v - 1);
    return shift >= 32 ? 0u : 1u << shift;
  };
  const u32 inner_height = height - b2, inner_width = width - b2;
  u32 pow2_height = pow2(inner_height);
  const u32 pow2_width = pow2(inner_width);
  u32 pow2_depth = dimension == 2 ? pow2(depth - b2) : 1;
  const bool packed_base = (pow2_width <= 16 || pow2_height <= 16) && (dword5 & 0x800);
  u32 address = (level == 0 || packed_base) ? (dword1 & ~0xFFFu) : (dword5 & ~0xFFFu);
  u32 row_pitch = 0, rows = 0, slices_of_level = 1;
  u32 out_width = width, out_height = height, out_depth = depth;
  if (level == 0 && !packed_base) {
    u32 w = width, h = height, d = depth;
    AlignTextureDimensions(w, h, d, bpp, format, dimension, tiled);
    rows = h;
    if (!tiled && !(dword1 & 0x400) && !(dword5 & 0x800) && dimension == 1 && !(dword4 & 0x3C0) &&
        !border) {
      u32 block_width, block_height;
      BlockSize(format, block_width, block_height);
      rows = (block_height + height - 1) & ~(block_height - 1);
    }
    slices_of_level = d;
    row_pitch = dimension == 0 ? (w * bpp) >> 3 : ((Extract(dword0, 22, 9) * bpp) << 2) & 0x1FFFFFFCu;
  } else {
    out_width = std::max(Shr(inner_width, level), 1u) + b2;
    out_height = std::max(Shr(inner_height, level), 1u) + b2;
    out_depth = dimension == 2 ? std::max(Shr(depth - b2, level), 1u) + b2 : 1;
    // Slices each level stores: cube faces, or stacked array slices in 4s.
    u32 stored_slices = 1;
    const u32 type = u32(t[0]) & 0xF;
    const bool cube = type == 3 && dimension == 3;
    const bool array = type == 3 && dimension == 1 && (dword1 & 0x400);
    if (cube || array) {
      const u32 align = 1u << ((dword1 >> 9) & 2);
      stored_slices = ((dword2 >> 26) + align) & ~(align - 1);
    }
    u32 log2_width = 31 - CountLeadingZeros(pow2_width);
    u32 log2_height = 31 - CountLeadingZeros(pow2_height);
    u32 log2_depth = 31 - CountLeadingZeros(pow2_depth);
    i32 remaining = i32(level);
    if (packed_base) {
      ++log2_width, ++log2_height, ++log2_depth;
    } else {
      --remaining;
    }
    while (remaining >= 0) {
      if (log2_width)
        --log2_width;
      if (log2_height)
        --log2_height;
      if (log2_depth)
        --log2_depth;
      u32 w = 1u << log2_width, h = 1u << log2_height, d = 1u << log2_depth;
      AlignTextureDimensions(w, h, d, bpp, format, dimension, tiled);
      row_pitch = (w * bpp) >> 3;
      if (((1u << log2_width) <= 16 || (1u << log2_height) <= 16) && (dword5 & 0x800)) {
        rows = h;
        slices_of_level = d;
        u32 x, y, z;
        out.tail_offset = MipTailOffset(u32(remaining), 1u << log2_width, 1u << log2_height,
                                        1u << log2_depth, row_pitch, row_pitch * h, format, x, y,
                                        z);
        address += out.tail_offset;
        break;
      }
      rows = h;
      slices_of_level = d;
      if (remaining > 0) {
        const u32 bytes = dimension == 2 ? (row_pitch * d * h + 0xFFF) & ~0xFFFu
                                         : ((row_pitch * h + 0xFFF) & ~0xFFFu) * d;
        address += bytes * stored_slices;
      }
      --remaining;
    }
  }
  const u32 plane = row_pitch * rows;
  const u32 plane_aligned = (plane + 0xFFF) & ~0xFFFu;
  out.address = plane_aligned * slice + address;
  out.row_pitch = row_pitch;
  if ((dword5 & 0x600) == 0x400) {  // 3D
    out.slice_bytes = plane;
    out.level_bytes = (plane * slices_of_level + 0xFFF) & ~0xFFFu;
  } else {
    out.slice_bytes = plane_aligned;
    out.level_bytes = plane_aligned * slices_of_level;
  }
  out.width = out_width;
  out.height = out_height;
  out.depth = out_depth;
  return out;
}


// ---------------------------------------------------------------------------
// Locks
// ---------------------------------------------------------------------------

// D3D's resource lock (sub_82654968): waits for the GPU to be done with the
// resource (its read fence for a read-only lock), notes the range the CPU
// writes (BaseFlush / MipFlush, in 128-byte lines) and counts the lock.
// Returns the address the CPU writes at.
u32 LockResource(u32 resource, u32 mip_lock, u32 base_address, u32 mip_address, u32 address,
                 u32 size, u32 flags) {
  auto *r = Ptr<be_u32>(resource);
  if (!r)
    return address;
  const u32 common = r[0];
  u32 fenced = resource;
  if ((common & 0xF) == 4 && (common & 0x40000000u) && u32(r[6]))
    fenced = r[6];  // a texture level: its texture
  const u32 fence = LoadU32(fenced + ((flags & 0x1010) ? 0xC : 0x8));
  if (fence)
    WaitForFence(fence);
  // Unless the lock is read-only or no-overwrite, D3D would then queue a GPU
  // cache invalidation of the range (into the ring, or for another thread a
  // range at device +0x2E38); the host has no GPU caches over guest memory.
  if ((flags & 0x10) || (common & 0x200000u)) {
    if (!(flags & 1)) {
      u32 base, flush_offset;
      if (mip_lock && mip_address) {
        base = mip_address & ~0x7Fu;
        flush_offset = 0x18;
      } else {
        base = base_address & ~0x7Fu;
        flush_offset = 0x14;
      }
      const u32 relative = address - base;
      const u32 first = relative >> 7;
      const u32 end = (relative + size + 0x7F) >> 7;
      const u32 range = LoadU32(resource + flush_offset);
      u32 new_end = range & 0xFFFF;
      u32 new_first = range >> 16;
      if (end > new_end)
        new_end = end;
      if (first < new_first)
        new_first = first;
      StoreU32(resource + flush_offset, (new_first << 16) | new_end);
    }
    if (flags & 0x10)
      address = GpuAddress(address) - 0x40000000u;  // the write-combined view
  }
  AtomicAddGuest(resource, 0x100);
  return address;
}

// void *D3DVertexBuffer_Lock(D3DVertexBuffer*, UINT OffsetToLock, UINT SizeToLock,
//     DWORD Flags) [0x82654E80]: the whole buffer.
u32 D3DVertexBuffer_Lock_hook(u32 buffer, u32 /*offset*/, u32 /*size*/, u32 flags) {
  const u32 base = LoadU32(buffer + 0x18) & ~3u;
  return LockResource(buffer, 0, base, 0, base, LoadU32(buffer + 0x1C) & 0x3FFFFFC, flags);
}

// void *D3DIndexBuffer_Lock(D3DIndexBuffer*, UINT OffsetToLock, UINT SizeToLock,
//     DWORD Flags) [0x82654F90]
u32 D3DIndexBuffer_Lock_hook(u32 buffer, u32 /*offset*/, u32 /*size*/, u32 flags) {
  const u32 base = LoadU32(buffer + 0x18);
  return LockResource(buffer, 0, base, 0, base, LoadU32(buffer + 0x1C), flags | 2);
}

// D3D::LockSurface(D3DBaseTexture*, ArrayIndex, Level, Flags, DWORD *pAddress,
//     DWORD *pRowPitch, DWORD *pSliceBytes, DWORD *pTailOffset) [0x8263E420]:
// the row pitch reported is per row of blocks. (The first release had an
// unused 0 second argument, which TU3's build dropped.)
void LockSurface(u32 texture, u32 slice, u32 level, u32 flags, u32 &address, u32 &row_pitch,
                 u32 &slice_bytes, u32 &tail_offset) {
  const Subresource sub = SubresourceLayout(texture, slice, level);
  const u32 dword1 = LoadU32(texture + 0x20);
  u32 block_width, block_height;
  BlockSize(dword1 & 0x3F, block_width, block_height);
  row_pitch = sub.row_pitch * block_height;
  slice_bytes = sub.slice_bytes;
  tail_offset = sub.tail_offset;
  address = LockResource(texture, level, dword1 & ~0xFFFu, LoadU32(texture + 0x30) & ~0xFFFu,
                         sub.address - sub.tail_offset, sub.level_bytes, flags) +
            sub.tail_offset;
}

// D3D::Lock2DSurface(D3DBaseTexture*, ArrayIndex, Level, D3DLOCKED_RECT*,
//     const RECT*, DWORD Flags) [0x8263E4E0] (TU3: no unused 0 second argument)
void D3D_Lock2DSurface_hook(u32 texture, u32 slice, u32 level, mapped_u32 locked_rect,
                            mapped_u32 rect, u32 flags) {
  u32 address, row_pitch, slice_bytes, tail_offset;
  LockSurface(texture, slice, level, flags, address, row_pitch, slice_bytes, tail_offset);
  if (rect) {
    const u32 bpp = FormatBitsPerPixel(LoadU32(texture + 0x20));
    address += ((bpp * u32(rect[0])) >> 3) + u32(rect[1]) * row_pitch;
  }
  if (locked_rect) {
    locked_rect[0] = row_pitch;  // Pitch
    locked_rect[1] = address;    // pBits
  }
}

// D3D::Lock3DSurface(D3DBaseTexture*, UINT Level, D3DLOCKED_BOX*, const D3DBOX*,
//     DWORD Flags) [0x826559A0]
void D3D_Lock3DSurface_hook(u32 texture, u32 level, mapped_u32 locked_box, mapped_u32 box,
                            u32 flags) {
  u32 address, row_pitch, slice_bytes, tail_offset;
  LockSurface(texture, 0, level, flags, address, row_pitch, slice_bytes, tail_offset);
  if (box) {
    const u32 bpp = FormatBitsPerPixel(LoadU32(texture + 0x20));
    address += ((bpp * u32(box[0])) >> 3) + u32(box[1]) * row_pitch + u32(box[4]) * slice_bytes;
  }
  if (locked_box) {
    locked_box[0] = row_pitch;    // RowPitch
    locked_box[1] = slice_bytes;  // SlicePitch
    locked_box[2] = address;      // pBits
  }
}

// The lock entry points, which only pick the texture, slice and level.
void D3DArrayTexture_LockRect_hook(u32 texture, u32 slice, u32 level, mapped_u32 locked_rect,
                                   mapped_u32 rect, u32 flags) {  // 0x82655DB0
  D3D_Lock2DSurface_hook(texture, slice, level, locked_rect, rect, flags);
}
void D3DLineTexture_LockRect_hook(u32 texture, u32 level, mapped_u32 locked_rect, mapped_u32 rect,
                                  u32 flags) {  // 0x82655EA8
  D3D_Lock2DSurface_hook(texture, 0, level, locked_rect, rect, flags);
}
void D3DSurface_LockRect_hook(u32 surface, mapped_u32 locked_rect, mapped_u32 rect,
                              u32 flags) {  // 0x82656150: a texture level's surface
  const u32 info = LoadU32(surface + 0x1C);
  D3D_Lock2DSurface_hook(LoadU32(surface + 0x18), Extract(info, 22, 6), info >> 28,
                         locked_rect, rect, flags);
}
void D3DVolume_LockBox_hook(u32 volume, mapped_u32 locked_box, mapped_u32 box,
                            u32 flags) {  // 0x826561C0
  D3D_Lock3DSurface_hook(LoadU32(volume + 0x18), LoadU32(volume + 0x1C) >> 28, locked_box, box,
                         flags);
}

// void D3D::UnlockResource(D3DResource *pResource, DWORD BaseAddress,
//     DWORD MipAddress) [0x82654C28]: the end of every Lock (D3DVertexBuffer_Unlock,
// D3DSurface_UnlockRect, D3DArrayTexture_UnlockRect...), after the CPU has
// written the data. The last unlock clears the written ranges, which D3D
// flushes from the CPU cache first. N&B's font cache (sub_82B8F848) writes
// each glyph between D3DLineTexture_LockRect and D3DArrayTexture_UnlockRect.
void D3D_UnlockResource_hook(u32 resource, u32 /*base_address*/, u32 mip_address) {
  const u32 old = AtomicAddGuest(resource, ~0xFFu);  // -0x100
  if ((old & 0xF00) == 0x100) {
    if (LoadU32(resource + 0x14) != 0xFFFF0000u)
      StoreU32(resource + 0x14, 0xFFFF0000u);
    if (mip_address && LoadU32(resource + 0x18) != 0xFFFF0000u)
      StoreU32(resource + 0x18, 0xFFFF0000u);
  }
}

// void D3DVertexBuffer_Unlock(D3DVertexBuffer*) [0x82654ED0]
void D3DVertexBuffer_Unlock_hook(u32 buffer) {
  D3D_UnlockResource_hook(buffer, LoadU32(buffer + 0x18) & ~3u, 0);
}

// void D3DSurface_UnlockRect(D3DSurface*) [0x826561E0]
void D3DSurface_UnlockRect_hook(u32 surface) {
  const u32 texture = LoadU32(surface + 0x18);
  D3D_UnlockResource_hook(texture, LoadU32(texture + 0x20) & ~0xFFFu,
                          LoadU32(texture + 0x30) & ~0xFFFu);
}


// ---------------------------------------------------------------------------
// Destruction
// ---------------------------------------------------------------------------

// D3D's teardown of a resource (the per-type cases of D3DResource_Destroy
// 0x822084F0): wait for the GPU to be done with it, free its data, then the header.
// The native D3D frees what it allocated; memory N&B's code allocated goes back to
// N&B's allocator (XMemFree).
void DestroyGuestResource(u32 resource) {
  auto* r = Ptr<be_u32>(resource);
  if (!r)
    return;
  const bool owned = OwnsHeader(resource);
  auto free_data = [owned](u32 va) {
    if (!va)
      return;
    if (owned)
      FreePhysical(va);
    else
      GameXMemFree(va, kAllocPhysical);
  };
  const u32 common = r[0];
  auto wait = [&] {
    if (const u32 fence = r[2])
      WaitForFence(fence);
  };
  switch (common & 0xF) {
  case 1:  // vertex buffer
    wait();
    free_data(u32(r[6]) & ~3u);
    break;
  case 2:  // index buffer
  case 7:  // pixel shader: its physical part
  case 8:  // constant buffer
    wait();
    free_data(r[6]);
    break;
  case 3:  // texture: base and mip data
    wait();
    free_data(u32(r[8]) & ~0xFFFu);
    free_data(u32(r[12]) & ~0xFFFu);
    break;
  case 4:  // surface (a texture level's: nothing of its own)
    if (!(common & 0x40000000u)) {
      if (common & 0x80000000u)
        FreeEdram(u32(r[7]) & 0xFFF, u32(r[11]) / 0x1400);
      if (u32(r[8]) & 1)
        StoreU32(kHiZOwnerVa, 0);
    }
    break;
  case 6:  // vertex shader: its physical part
    wait();
    free_data(r[8]);
    break;
  case 9:  // command buffer: its memory, unless the caller owns it
    wait();
    if (u32(r[0x98 / 4]) && !(u32(r[0x6C / 4]) & 0x10000))
      free_data(r[0x98 / 4]);
    break;
  default:
    break;
  }
  if (owned)
    FreeHeader(resource);
  else
    GameXMemFree(resource, kAllocHeader);
}

// _GetTextureDimensions (0x82207AF8): from dword 2, by dimension, with the
// border's two texels.
void TextureDimensions(u32 texture, u32 &width, u32 &height, u32 &depth) {
  const u32 size = LoadU32(texture + 0x24);
  const u32 add = ((LoadU32(texture + 0x28) >> 31) << 1) + 1;
  switch (Extract(LoadU32(texture + 0x30), 9, 2)) {
    case 0:
      width = (size & 0xFFFFFF) + add;
      height = depth = 1;
      break;
    case 2:
      width = (size & 0x7FF) + add;
      height = ((size >> 11) & 0x7FF) + add;
      depth = (size >> 22) + add;
      break;
    default:
      width = (size & 0x1FFF) + add;
      height = ((size >> 13) & 0x1FFF) + add;
      depth = 1;
      break;
  }
}

struct LevelSize {
  u32 width = 0, height = 0, depth = 0, pitch = 0, size = 0;
};

// sub_82207D48(D3DBaseTexture*, UINT Level, ...): a level's dimensions, its
// row pitch and its size, as the descriptions report them.
LevelSize LevelLayout(u32 texture, u32 level) {
  LevelSize out;
  const auto *t = Ptr<const be_u32>(texture);
  if (!t)
    return out;
  const u32 dword0 = t[7], dword1 = t[8], dword3 = t[10], dword4 = t[11], dword5 = t[12];
  const u32 format = dword1 & 0x3F;
  const u32 bpp = FormatBitsPerPixel(dword1);
  const u32 type = ResourceTypeOf(texture);
  const u32 dimension = Extract(dword5, 9, 2);
  const bool tiled = (dword0 >> 31) != 0;
  u32 width, height, depth;
  TextureDimensions(texture, width, height, depth);
  u32 aw, ah, ad, pitch;
  auto header_pitch = [&] { return ((Extract(dword0, 22, 9) * bpp) & 0x07FFFFFFu) << 2; };
  if (type == 4) {
    aw = width, ah = height, ad = depth;
    AlignTextureDimensions(aw, ah, ad, bpp, format, dimension, tiled);
    pitch = header_pitch();
  } else {
    const u32 border = dword3 >> 31;
    const u32 b2 = border * 2;
    auto pow2 = [border](u32 v) { return Slw(1, border + 32 - CountLeadingZeros(v - 1)); };
    const u32 pow2_width = pow2(width - b2);
    const u32 pow2_height = pow2(height - b2);
    const u32 pow2_depth = dimension == 2 ? pow2(depth - b2) : 1;
    if (level == 0) {
      if ((pow2_width <= 16 || pow2_height <= 16) && (dword5 & 0x800)) {
        aw = pow2_width, ah = pow2_height, ad = pow2_depth;
      } else {
        aw = width, ah = height, ad = depth;
      }
      AlignTextureDimensions(aw, ah, ad, bpp, format, dimension, tiled);
      if (!tiled && !(dword1 & 0x400) && !(dword5 & 0x800) && dimension == 1 && !(dword4 & 0x3C0) &&
          !border) {
        u32 block_width, block_height;
        BlockSize(format, block_width, block_height);
        ah = (block_height + height - 1) & ~(block_height - 1);
      }
      pitch = header_pitch();
    } else {
      const u32 level_width = std::max(Shr(width - b2, level), 1u) + b2;
      const u32 level_height = std::max(Shr(height - b2, level), 1u) + b2;
      const u32 level_depth = dimension == 2 ? std::max(Shr(depth - b2, level), 1u) + b2 : 1;
      if (dword5 & 0x800) {  // packed mips: the tail's level at most
        const i32 log_width = i32(border + 32 - CountLeadingZeros(width - b2 - 1));
        const i32 log_height = i32(border + 32 - CountLeadingZeros(height - b2 - 1));
        const i32 tail = std::max(std::min(log_width, log_height) - 4, 0);
        if (level >= u32(tail))
          level = u32(tail);
      }
      width = level_width, height = level_height, depth = level_depth;
      aw = std::max(Shr(pow2_width, level), 1u);
      ah = std::max(Shr(pow2_height, level), 1u);
      ad = std::max(Shr(pow2_depth, level), 1u);
      AlignTextureDimensions(aw, ah, ad, bpp, format, dimension, tiled);
      pitch = (aw * bpp) >> 3;
    }
  }
  out.width = width;
  out.height = height;
  out.depth = depth;
  out.pitch = pitch;
  out.size = pitch * ah;
  if (!(!tiled && level <= 1 && (type == 3 || type == 0x14)) && dimension != 2)
    out.size = (out.size + 0xFFF) & ~0xFFFu;
  return out;
}

// The D3DFORMAT a texture header describes (Get2DSurfaceDesc's): the GPU
// format and its flags from dword 1, the swizzle from dword 3, the sign and
// endian bits from dword 0.
u32 HeaderFormat(u32 texture) {
  const u32 dword0 = LoadU32(texture + 0x1C), dword1 = LoadU32(texture + 0x20);
  const u32 dword3 = LoadU32(texture + 0x28);
  u32 swizzle = (((dword3 >> 10) & 7) << 3) | ((dword3 >> 7) & 7);
  swizzle = (swizzle << 3) | ((dword3 >> 4) & 7);
  swizzle = (swizzle << 3) | ((dword3 >> 1) & 7);
  u32 f = (swizzle << 1) | (dword3 & 1);
  f = (f << 2) | ((dword0 >> 8) & 3);
  f = (f << 2) | ((dword0 >> 6) & 3);
  f = (f << 2) | ((dword0 >> 4) & 3);
  f = (f << 2) | ((dword0 >> 2) & 3);
  f = (f << 1) | (dword0 >> 31);
  f = (f << 2) | ((dword1 >> 6) & 3);
  return (f << 6) | (dword1 & 0x3F);
}

// void Get2DSurfaceDesc(D3DBaseTexture*, UINT Level, D3DSURFACE_DESC*) [0x822081A0]
void Get2DSurfaceDesc(u32 texture, u32 level, mapped_u32 desc) {
  if (!desc)
    return;
  const LevelSize layout = LevelLayout(texture, level);
  desc[0] = HeaderFormat(texture);
  desc[1] = ResourceTypeOf(texture);
  desc[2] = 0u;  // Usage
  desc[3] = 0u;  // Pool
  desc[5] = 0u;  // MultiSampleQuality
  desc[6] = layout.width;
  desc[7] = layout.height;
  desc[4] = 0u;  // MultiSampleType
}

// void Get3DSurfaceDesc(D3DBaseTexture*, UINT Level, D3DVOLUME_DESC*) [0x82655738]
void Get3DSurfaceDesc(u32 texture, u32 level, mapped_u32 desc) {
  if (!desc)
    return;
  const LevelSize layout = LevelLayout(texture, level);
  desc[0] = HeaderFormat(texture);
  desc[1] = ResourceTypeOf(texture);
  desc[2] = 0u;
  desc[3] = 0u;
  desc[4] = layout.width;
  desc[5] = layout.height;
  desc[6] = layout.depth;
}

// void D3DSurface_GetDesc(D3DSurface*, D3DSURFACE_DESC*) [0x822082A0]
void D3DSurface_GetDesc_hook(u32 surface, mapped_u32 desc) {
  if (!desc)
    return;
  const u32 common = LoadU32(surface);
  if (common & kCommonTextureLevelBit) {
    Get2DSurfaceDesc(LoadU32(surface + 0x18), LoadU32(surface + 0x1C) >> 28, desc);
  } else {
    const u32 size = LoadU32(surface + 0x24);
    desc[2] = 0u;
    desc[3] = 0u;
    desc[5] = 0u;
    desc[0] = LoadU32(surface + 0x28);
    desc[6] = (size >> 18) + 1;
    desc[7] = Extract(size, 3, 15) + 1;
    desc[4] = (LoadU32(surface + 0x18) >> 16) & 3;
  }
  desc[1] = 4u;  // D3DRTYPE_SURFACE
}

// void D3DVolume_GetDesc(D3DVolume*, D3DVOLUME_DESC*) [0x82656178]
void D3DVolume_GetDesc_hook(u32 volume, mapped_u32 desc) {
  if (!desc)
    return;
  Get3DSurfaceDesc(LoadU32(volume + 0x18), LoadU32(volume + 0x1C) >> 28, desc);
  desc[1] = 0x10u;  // D3DRTYPE_VOLUME
}

// A level's header (0x30 bytes for a surface, 0x20 for a volume) over a
// texture, which it references: the texture levels' Common (0x40100004,
// the texture's 0x200000 bit), the level in dword 7's top nibble.
u32 LevelHeader(u32 texture, u32 size, u32 info) {
  if (!Ptr<u8>(texture))
    return 0;
  const u32 header = AllocHeader(size);
  if (!header)
    return 0;
  auto *h = Ptr<be_u32>(header);
  h[0] = 0x40100004u | (LoadU32(texture) & 0x200000u);
  h[1] = 1u;
  h[5] = 0xFFFF0000u;
  h[6] = texture;
  h[7] = info;
  AddRef(texture);
  return header;
}

// D3DSurface *D3DTexture_GetSurfaceLevel(D3DTexture*, UINT Level) [0x82208928]
u32 D3DTexture_GetSurfaceLevel_hook(u32 texture, u32 level) {
  return LevelHeader(texture, 0x30, level << 28);
}

// D3DSurface *D3DCubeTexture_GetCubeMapSurface(D3DCubeTexture*, D3DCUBEMAP_FACES,
//     UINT Level) [0x82655CD0]
u32 D3DCubeTexture_GetCubeMapSurface_hook(u32 texture, u32 face, u32 level) {
  return LevelHeader(texture, 0x30, (level << 28) | ((face & 0x3F) << 22));
}

// D3DVolume *D3DVolumeTexture_GetVolumeLevel(D3DVolumeTexture*, UINT Level) [0x82655DD8]
u32 D3DVolumeTexture_GetVolumeLevel_hook(u32 texture, u32 level) {
  return LevelHeader(texture, 0x20, level << 28);
}

// void D3DArrayTexture_UnlockRect(D3DArrayTexture*, ...) [0x82655D88]
void D3DArrayTexture_UnlockRect_hook(u32 texture) {
  D3D_UnlockResource_hook(texture, LoadU32(texture + 0x20) & ~0xFFFu,
                          LoadU32(texture + 0x30) & ~0xFFFu);
}

// void D3DConstantBuffer_Unlock(D3DConstantBuffer*) [0x822693A0]
void D3DConstantBuffer_Unlock_hook(u32 buffer) {
  D3D_UnlockResource_hook(buffer, LoadU32(buffer + 0x18), 0);
}

// ---------------------------------------------------------------------------
// Buffers and textures
// ---------------------------------------------------------------------------

// The XMemAlloc attributes of buffer data (0x82800000, write-combined, or
// cached for D3DUSAGE_CPU_CACHED_MEMORY) and Common's usage bits.
u32 BufferAttributes(u32 usage) {
  return 0x82800000u | ((usage & 4) ? 0x20000000u : 0x30000000u);
}

// D3DVertexBuffer *D3DDevice_CreateVertexBuffer(UINT Length, DWORD Usage,
//     D3DPOOL) [0x82654DB8]
u32 D3DDevice_CreateVertexBuffer_hook(u32 length, u32 usage, u32 /*pool*/) {
  const u32 header = AllocHeader(0x20);
  if (!header)
    return 0;
  u32 common = (usage & 4) ? 0x300001u : 0x100001u;
  if (usage & 0x200)
    common |= 0x400000u;
  const u32 data = AllocPhysical(length, BufferAttributes(usage));
  if (!data) {
    FreeHeader(header);
    return 0;
  }
  auto *h = Ptr<be_u32>(header);
  h[0] = common;
  h[6] = data | 3;  // the data, endian 8in32
  h[1] = 1u;
  h[5] = 0xFFFF0000u;
  h[7] = (length & 0x03FFFFFCu) | (u32(h[7]) & 0x08000000u) | 0x10000002u;
  return header;
}

// D3DIndexBuffer *D3DDevice_CreateIndexBuffer(UINT Length, DWORD Usage,
//     D3DFORMAT, D3DPOOL) [0x82654EE0]
u32 D3DDevice_CreateIndexBuffer_hook(u32 length, u32 usage, u32 format) {
  const u32 header = AllocHeader(0x20);
  if (!header)
    return 0;
  u32 common = (format << 29) | 0x100002u;
  if (usage & 4)
    common |= 0x200000u;
  if (usage & 0x200)
    common |= 0x400000u;
  const u32 data = AllocPhysical(length, BufferAttributes(usage));
  if (!data) {
    FreeHeader(header);
    return 0;
  }
  auto *h = Ptr<be_u32>(header);
  h[7] = length;
  h[0] = common;
  h[1] = 1u;
  h[5] = 0xFFFF0000u;
  h[6] = data;
  return header;
}

// _GetMaxMipLevels (0x826554B8): the levels down to 1x1x1 of the largest
// dimension, without the border.
u32 GetMaxMipLevels(u32 width, u32 height, u32 depth, u32 border) {
  const u32 b2 = border * 2;
  const u32 d = depth - b2;
  const u32 dm = i32(d) > 1 ? d : 1;
  const u32 h = height - b2;
  const u32 hm = h > dm ? h : dm;
  const u32 w = width - b2;
  return 32 - CountLeadingZeros(w > hm ? w : hm);
}

// _FindTextureSize (0x82287E60): the aligned width of the base level (in
// texels), its bytes and the mip levels' bytes. Returns the level count (all
// of them for 0).
u32 FindTextureSize(u32 width, u32 height, u32 depth, u32 levels, u32 format, u32 dimension,
                    bool tiled, bool packed, u32 border, u32 pitch, u32 &out_width,
                    u32 &out_base, u32 &out_mips) {
  const bool array = dimension == 1 && depth > 1;
  const bool slices = array || dimension == 3;  // array slices or cube faces
  const u32 base_depth = slices ? 1 : depth;
  const u32 bpp = FormatBitsPerPixel(format);
  const u32 b2 = border * 2;
  const u32 inner_width = width - b2, inner_height = height - b2;
  u32 log_width = border + 32 - CountLeadingZeros(inner_width - 1);
  u32 log_height = border + 32 - CountLeadingZeros(inner_height - 1);
  u32 inner_depth = 1, log_depth = 0;
  if (dimension == 2) {
    inner_depth = base_depth - b2;
    log_depth = border + 32 - CountLeadingZeros(inner_depth - 1);
  }
  u32 aw = Slw(1, log_width), ah = Slw(1, log_height), ad = Slw(1, log_depth);
  if (!((aw <= 16 || ah <= 16) && packed)) {
    aw = pitch && bpp ? (pitch * 8) / bpp : width;  // (the original traps on 0 bits per pixel)
    ah = height;
    ad = base_depth;
  }
  AlignTextureDimensions(aw, ah, ad, bpp, format, dimension, tiled);
  out_width = aw;
  if (!pitch)
    pitch = (aw * bpp) >> 3;
  u32 base;
  u32 mips = 0;
  bool linear_2d = false;
  if (dimension == 2) {
    base = (pitch * ad * ah + 0xFFF) & ~0xFFFu;
  } else if (!tiled && !slices) {
    if (dimension == 1 && levels == 1 && !packed && !border) {
      u32 block_width, block_height;
      BlockSize(format, block_width, block_height);
      base = ((block_height + height - 1) & ~(block_height - 1)) * pitch;
      linear_2d = true;
    } else {
      base = pitch * ad * ah;
    }
  } else {
    base = ((pitch * ah + 0xFFF) & ~0xFFFu) * ad;
  }
  if (!linear_2d) {
    if (slices)
      base *= depth;
    if (levels > 1 || levels == 0) {
      const u32 hd = inner_height > inner_depth ? inner_height : inner_depth;
      const u32 largest = inner_width > hd ? inner_width : hd;
      if (!levels)
        levels = 32 - CountLeadingZeros(largest);
      u32 level_depth = base_depth;
      for (u32 n = levels - 1; n; --n) {
        if (packed && (log_width <= 4 || log_height <= 4))
          break;  // the rest are in the packed tail
        if (log_width)
          --log_width;
        if (log_height)
          --log_height;
        if (level_depth > 1)
          level_depth >>= 1;
        u32 mw = Slw(1, log_width), mh = Slw(1, log_height), md = level_depth;
        mips += AlignTextureDimensions(mw, mh, md, bpp, format, dimension, tiled);
      }
      if (slices) {
        const u32 align = array ? 4 : 1;
        mips *= (depth + align - 1) & ~(align - 1);
      }
    }
  }
  out_base = base;
  out_mips = mips;
  return levels;
}

// D3D::SetTextureHeader(D3DRESOURCETYPE, Width, Height, Depth, Levels, Usage,
//     D3DFORMAT, MIPPACKINGTYPE, Border, ExpAdjust, Pitch, D3DBaseTexture*,
//     DWORD *pBaseSize, DWORD *pMipSize) [0x8228E990]: the header and fetch
// constant of a texture; the sizes of its base and mip data.
void SetTextureHeader(u32 type, u32 width, u32 height, u32 depth, u32 levels, u32 usage,
                      u32 format, u32 packing, u32 border, u32 exp_adjust, u32 pitch,
                      u32 header_va, u32 &base_size, u32 &mip_size) {
  auto *t = Ptr<be_u32>(header_va);
  if (!t)
    return;
  const u32 add = border * 2 + 1;
  u32 dimension = 1, max_depth = 1;
  switch (type) {
    case 0x11: dimension = 2; max_depth = depth; break;  // volume
    case 0x12: dimension = 3; break;                     // cube
    case 0x14: dimension = 0; packing = 0; break;        // line
    default: dimension = 1; break;                       // 2D and array
  }
  if (!levels)
    levels = GetMaxMipLevels(width, height, max_depth, border);
  const u32 packed = packing == 2 ? (levels > 1 ? 1u : 0u) : packing;
  u32 aligned_width = 0, base = 0, mips = 0;
  FindTextureSize(width, height, depth, levels, format & 0x3F, dimension, (format >> 8) & 1,
                  packed != 0, border, pitch, aligned_width, base, mips);

  u32 common = (usage & 4) ? 0x300003u : 0x100003u;
  if (usage & 0x200)
    common |= 0x400000u;
  t[5] = 0xFFFF0000u;  // BaseFlush
  u32 dword5 = Insert(t[12], dimension, 9, 2);
  t[0] = common;
  t[1] = 1u;
  t[6] = 0xFFFF0000u;  // MipFlush
  t[2] = 0u;
  t[12] = dword5;
  // dword 2: the size, by dimension.
  if (dimension == 0) {
    t[9] = (u32(t[9]) & 0xFF000000u) | ((width - add) & 0x00FFFFFFu);
  } else if (dimension == 1) {
    const u32 size = (u32(t[9]) & 0xFFFFE000u) | ((width - add) & 0x1FFF);
    t[9] = (((height - add) & 0x1FFF) << 13) | ((depth - 1) << 26) | (size & 0x1FFF);
  } else if (dimension == 2) {
    const u32 size = (u32(t[9]) & 0xFFFFF800u) | ((width - add) & 0x7FF);
    t[9] = (((height - add) & 0x7FF) << 11) | ((depth - add) << 22) | (size & 0x7FF);
  } else {
    t[9] = Insert(Insert(width - add, height - add, 13, 13), depth - 1, 26, 6);
  }
  // dwords 0, 1, 3, 4: the format's fields (sign, endian, swizzle...), the
  // pitch, the levels, the border and ExpAdjust.
  const i32 f = i32(format);
  u32 r9 = u32(f >> 15), r8 = aligned_width, r7 = u32(f >> 13), r6 = t[7], r5 = u32(f >> 11);
  const u32 r4 = t[8];
  u32 r3 = u32(f >> 9);
  const u32 r30 = t[10];
  u32 r29 = u32(f >> 8), r28 = t[11], r27 = u32(f >> 6), r24 = u32(f >> 27) & 7;
  u32 r20 = border << 21;
  r7 = Insert(r7, r9, 2, 2);
  r24 |= r20;
  u32 is_array = CountLeadingZeros(type - 0x13);
  r8 = Insert(r8, r29, 14, 18);
  r7 &= 0xF;
  r20 = u32(f >> 24);
  r24 = Insert(r24, exp_adjust, 3, 6);
  r5 = Insert(r5, r7, 2, 30);
  r6 = (r6 & 0x003FFFFFu) | (std::rotl(r8, 17) & 0xFFC00000u);
  is_array = (is_array >> 5) & 1;
  r29 = u32(f >> 21);
  r20 = Insert(r20, r24, 3, 29);
  r27 = Insert(r27, is_array, 4, 1);
  r3 = Insert(r3, r5, 2, 30);
  r7 = r6;
  r8 = u32(f >> 18);
  r29 = Insert(r29, r20, 3, 29);
  u32 dword1 = r4 & ~0x3Fu;
  r8 = Insert(r8, r29, 3, 29);
  r6 = r3 << 2;
  r7 &= ~0x3FFu;
  r5 = u32(f >> 17);
  dword1 |= format & 0x3F;
  r7 |= r6;
  r5 = Insert(r5, r8, 1, 31);
  dword1 = Insert(dword1, r27, 6, 2);
  r8 = r30 & 0x7FF80000u;
  r7 |= 2;
  r28 = Insert(r28, levels - 1, 6, 4);
  dword1 = (dword1 & ~0x400u) | (std::rotl(r27, 6) & 0x400u);
  t[7] = r7;
  dword5 = Insert(dword5, packed, 11, 1);
  t[11] = r28;
  t[8] = dword1;
  t[12] = dword5;
  t[10] = r8 | r5;
  base_size = base;
  mip_size = mips;
}

// D3DBaseTexture *D3DDevice_CreateTexture(UINT Width, UINT Height, UINT Depth,
//     UINT Levels, DWORD Usage, D3DFORMAT, D3DPOOL, D3DRESOURCETYPE) [0x82655EC8]
u32 D3DDevice_CreateTexture_hook(u32 width, u32 height, u32 depth, u32 levels, u32 usage,
                                 u32 format, u32 /*pool*/, u32 type) {
  const u32 header = AllocHeader(0x34);
  if (!header)
    return 0;
  u32 base_size = 0, mip_size = 0;
  SetTextureHeader(type, width, height, depth, levels, usage, format, 2, 0, 0, 0, header, base_size,
                   mip_size);
  // Write-combined, or cached for D3DUSAGE_CPU_CACHED_MEMORY; 4 KiB aligned.
  const u32 attributes = 0x8C800000u | ((((~usage >> 2) & 1) | 2) << 28);
  const u32 base = AllocPhysical(base_size, attributes);
  if (!base) {
    FreeHeader(header);
    return 0;
  }
  u32 mips = 0;
  if (mip_size) {
    mips = AllocPhysical(mip_size, attributes);
    if (!mips) {
      FreeHeader(header);
      FreePhysical(base);
      return 0;
    }
  }
  auto *t = Ptr<be_u32>(header);
  t[8] = (u32(t[8]) & 0xFFFu) | (base & ~0xFFFu);
  t[12] = (mips & ~0xFFFu) | (u32(t[12]) & 0xFFFu);
  return header;
}

}  // namespace

// ---------------------------------------------------------------------------
// The calls (resource.h)
// ---------------------------------------------------------------------------

void SetGuestCalls(const GuestCalls& calls) { g_calls = calls; }

SubresourceInfo QuerySubresource(u32 texture, u32 slice, u32 level) {
  const Subresource sub = SubresourceLayout(texture, slice, level);
  return {sub.address, sub.row_pitch, sub.slice_bytes, sub.level_bytes, sub.tail_offset,
          sub.width,   sub.height,    sub.depth};
}

u32 CreateVertexBuffer(u32 length, u32 usage) { return D3DDevice_CreateVertexBuffer_hook(length, usage, 0); }
u32 CreateIndexBuffer(u32 length, u32 usage, u32 format) {
  return D3DDevice_CreateIndexBuffer_hook(length, usage, format);
}
u32 CreateTexture(u32 width, u32 height, u32 depth, u32 levels, u32 usage, u32 format, u32 type) {
  return D3DDevice_CreateTexture_hook(width, height, depth, levels, usage, format, 0, type);
}
u32 CreateSurface(u32 width, u32 height, u32 format, u32 multisample, u32 parameters_va) {
  return CreateSurfaceImpl(width, height, format, multisample, parameters_va);
}

u32 LockVertexBuffer(u32 buffer, u32 flags) { return D3DVertexBuffer_Lock_hook(buffer, 0, 0, flags); }
u32 LockIndexBuffer(u32 buffer, u32 flags) { return D3DIndexBuffer_Lock_hook(buffer, 0, 0, flags); }
void LockArrayTextureRect(u32 texture, u32 slice, u32 level, u32 locked_rect, u32 rect, u32 flags) {
  D3DArrayTexture_LockRect_hook(texture, slice, level, Mapped(Ptr<be_u32>(locked_rect)),
                                Mapped(Ptr<be_u32>(rect)), flags);
}
void LockLineTextureRect(u32 texture, u32 level, u32 locked_rect, u32 rect, u32 flags) {
  D3DLineTexture_LockRect_hook(texture, level, Mapped(Ptr<be_u32>(locked_rect)),
                               Mapped(Ptr<be_u32>(rect)), flags);
}
void LockSurfaceRect(u32 surface, u32 locked_rect, u32 rect, u32 flags) {
  D3DSurface_LockRect_hook(surface, Mapped(Ptr<be_u32>(locked_rect)), Mapped(Ptr<be_u32>(rect)), flags);
}
void LockVolumeBox(u32 volume, u32 locked_box, u32 box, u32 flags) {
  D3DVolume_LockBox_hook(volume, Mapped(Ptr<be_u32>(locked_box)), Mapped(Ptr<be_u32>(box)), flags);
}

void UnlockVertexBuffer(u32 buffer) { D3DVertexBuffer_Unlock_hook(buffer); }
void UnlockSurfaceRect(u32 surface) { D3DSurface_UnlockRect_hook(surface); }
void UnlockArrayTextureRect(u32 texture) { D3DArrayTexture_UnlockRect_hook(texture); }
void UnlockConstantBuffer(u32 buffer) { D3DConstantBuffer_Unlock_hook(buffer); }

void DestroyResource(u32 resource) { DestroyGuestResource(resource); }

void GetSurfaceDesc(u32 surface, u32 desc) { D3DSurface_GetDesc_hook(surface, Mapped(Ptr<be_u32>(desc))); }
void GetVolumeDesc(u32 volume, u32 desc) { D3DVolume_GetDesc_hook(volume, Mapped(Ptr<be_u32>(desc))); }
u32 GetSurfaceLevel(u32 texture, u32 level) { return D3DTexture_GetSurfaceLevel_hook(texture, level); }
u32 GetCubeMapSurface(u32 texture, u32 face, u32 level) {
  return D3DCubeTexture_GetCubeMapSurface_hook(texture, face, level);
}
u32 GetVolumeLevel(u32 texture, u32 level) { return D3DVolumeTexture_GetVolumeLevel_hook(texture, level); }

}  // namespace renut::d3d::port
