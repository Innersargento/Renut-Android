// Shaders, natively (see shader.h). Every offset is the retail device's or the
// retail shader object's; the device ones are in the part of the device Layout()
// leaves alone.
#include "d3d/resources/shader.h"

#include <cstring>

#include "d3d/device/internal.h"
#include "d3d/resources/microcode.h"

namespace renut::d3d::port {

namespace {

constexpr u32 kVertexShaderSlot = 0x3198;  // D3DDevice::m_pVertexShader
constexpr u32 kPixelShaderSlot = 0x3194;   // m_pPixelShader
constexpr u32 kFenceTag = 0x2A9C;
constexpr u32 kCommandBufferTagMask = 0x2AA0;  // Common bits a recorded buffer tracks
constexpr u32 kFlags2 = 0x2ABE;                // 0x80: a vertex shader is bound for the draw
constexpr u32 kConstants = 0x480;              // m_Constants: fetch constants, then the ALU ones
constexpr u32 kDeviceSize = 0x5E00;            // the device allocation

// A shader object is a resource header followed by its container (the microcode's
// virtual part): the vertex shader's at +0x368, the pixel shader's at +0x28.
constexpr u32 kVertexContainer = 0x368;
constexpr u32 kPixelContainer = 0x28;

constexpr u32 kAttributesObject = 0x24800000;    // XMemAlloc attributes of a shader object
constexpr u32 kAttributesMicrocode = 0xB5800000;  // ... of its physical part
// Function pointers N&B's D3D calls first when set (a debug validation hook); a
// creation with one set is left to the original.
constexpr u32 kVertexShaderValidator = 0x82FA36BC;
constexpr u32 kPixelShaderValidator = 0x82FA36C0;

u8* Guest(const u8* base, u32 address) {
  return renut::GuestPtr(const_cast<u8*>(base), address);
}

u32 Load(const u8* base, u32 address) { return LoadBE32(Guest(base, address)); }
void Store(const u8* base, u32 address, u32 value) { StoreBE32(Guest(base, address), value); }

// The constants a shader defines for the device (the block at container + [container
// + 0x14]): the dirty bits of the ALU constants it covers are cleared, and its
// tables write the device's constants: first a table that is skipped, then runs of
// words copied over the constants, then runs of (and, or) pairs applied to them.
void ApplyShaderConstants(Device d, u32 container, u32 pending_word) {
  const u32 offset = d.GuestW(container + 0x14);
  if (!offset)
    return;
  const u32 block = container + offset;
  const u64 covered = LoadBE64(Guest(d.base, block));
  d.SetPending(pending_word, d.Pending(pending_word) & ~covered);
  if (LoadBE64(Guest(d.base, block + 8)) != 0)
    d.Dirty(4, u64(1) << 56);
  u32 p = block + 0x14;
  const u32 size = d.GuestW(block + 0x10);
  if (size > (1u << 20))
    return;
  const u32 end = p + size;
  if (p >= end)
    return;
  // Runs that are skipped: (u16, u16 count, u32) until one with a zero count.
  for (;;) {
    const u32 count = LoadBE16(Guest(d.base, p + 2));
    p += 4;
    if (count == 0)
      break;
    p += 4;
    if (p >= end)
      break;
  }
  if (p >= end)
    return;
  // Runs copied: (u16 offset, u16 dwords), the dwords; until a zero count.
  for (;;) {
    const u32 target = LoadBE16(Guest(d.base, p));
    const u32 count = LoadBE16(Guest(d.base, p + 2));
    p += 4;
    if (count == 0)
      break;
    const u32 bytes = count * 4;
    if (kConstants + target + bytes <= kDeviceSize)
      std::memcpy(d.bytes + kConstants + target, Guest(d.base, p), bytes);
    p += bytes;
    if (p >= end)
      return;
  }
  if (p >= end)
    return;
  // Runs applied: (u16 offset, u16 dwords), pairs of (and mask, or value).
  for (;;) {
    u32 target = LoadBE16(Guest(d.base, p));
    u32 count = LoadBE16(Guest(d.base, p + 2));
    p += 4;
    if (count == 0)
      return;
    u32 at = kConstants + target;
    do {
      if (at + 4 <= kDeviceSize) {
        const u32 keep = Load(d.base, p), set = Load(d.base, p + 4);
        d.SetW(at, (d.W(at) & keep) | set);
      }
      p += 8;
      at += 4;
      count = (count - 2) & 0xFFFF;
    } while (count != 0);
    if (p >= end)
      return;
  }
}

// What the device does with the shader it stops using: it is busy until the current
// fence. (A device recording a command buffer adds it to the buffer's list instead,
// which the recompiled code does.)
bool RetireShader(Device d, u32 previous) {
  if (!previous)
    return true;
  const u32 tag = d.W(kFenceTag);
  if (tag) {
    Store(d.base, previous + 8, tag);
    return true;
  }
  return !(d.W(kCommandBufferTagMask) & Load(d.base, previous));
}

}  // namespace

bool SetVertexShader(Device d, u32 shader) {
  if (!d)
    return false;
  const u32 previous = d.W(kVertexShaderSlot);
  if (!RetireShader(d, previous))
    return false;
  if (shader)
    d.Dirty(2, 0x80000);
  d.SetB(kFlags2, d.B(kFlags2) & 0x7F);
  d.SetW(kVertexShaderSlot, shader);
  if (shader)
    ApplyShaderConstants(d, shader + kVertexContainer, 0);
  return true;
}

bool SetPixelShader(Device d, u32 shader) {
  if (!d)
    return false;
  const u32 previous = d.W(kPixelShaderSlot);
  if (!RetireShader(d, previous))
    return false;
  d.SetW(kPixelShaderSlot, shader);
  d.Dirty(2, 0x100000);
  d.Dirty(2, 0x20000);
  if (shader)
    ApplyShaderConstants(d, shader + kPixelContainer, 1);
  return true;
}

void RegisterVertexShader(uint8_t* base, u32 shader, u32 physical) {
  ForgetBoundFetches(shader);
  std::memset(Guest(base, shader), 0, kVertexContainer);
  Store(base, shader + 0x20, physical);
  Store(base, shader + 0, 6);  // D3DRTYPE_VERTEXSHADER
  Store(base, shader + 4, 1);
  Store(base, shader + 0x14, 0xFFFF0000u);
  // A shader with the container's 0x20 flag has a second program.
  const u32 programs = (Load(base, shader + kVertexContainer) & 0x20) ? 2 : 1;
  u32 program = shader + 0x28;
  u32 entry = shader + 0x380;
  for (u32 i = 0; i < programs; ++i) {
    const u32 instance = shader + Load(base, entry);
    const u32 container = instance + kVertexContainer;
    const u32 microcode = physical + Load(base, instance + kVertexContainer);
    const u32 table = container + (Load(base, instance + 0x380) + 9) * 4;
    const u32 count = Load(base, instance + 0x384);
    if (count) {
      u32 target = program + 0x1C;
      for (u32 k = 0; k < count; ++k, target += 0xC) {
        const u32 source = microcode + (Load(base, table + k * 4) & 0xFFF) * 0xC;
        if (target != source)
          std::memmove(Guest(base, target), Guest(base, source), 0xC);
      }
    }
    Store(base, program, 0xFFFFFFFFu);
    entry += 8;
    program += 0x1A0;
  }
}

void RegisterPixelShader(uint8_t* base, u32 shader, u32 physical) {
  std::memset(Guest(base, shader), 0, 0x28);
  Store(base, shader + 0, 7);  // D3DRTYPE_PIXELSHADER
  Store(base, shader + 4, 1);
  Store(base, shader + 0x14, 0xFFFF0000u);
  Store(base, shader + 0x18, physical);
}

bool CreateVertexShader(uint8_t* base, u32 function, const ShaderCalls& calls, u32& result) {
  result = 0;
  if (Load(base, kVertexShaderValidator))
    return false;
  const u32 virtual_size = Load(base, function + 4);
  const u32 physical_size = Load(base, function + 8);
  // N&B's vertex shaders carry the container's low flag; the object is laid out for
  // them (header + 0x340 + container). Anything else is the original's.
  if (!(Load(base, function) & 1) || virtual_size > (1u << 20) || physical_size > (1u << 20))
    return false;
  const u32 header = calls.xmem_alloc(calls.user, 0x28 + 0x340 + virtual_size, kAttributesObject);
  if (!header)
    return true;
  std::memset(Guest(base, header), 0, kVertexContainer);
  std::memcpy(Guest(base, header + kVertexContainer), Guest(base, function), virtual_size);
  const u32 physical = calls.xmem_alloc(calls.user, physical_size, kAttributesMicrocode);
  if (!physical) {
    calls.xmem_free(calls.user, header, kAttributesObject);
    return true;
  }
  std::memcpy(Guest(base, physical), Guest(base, function + virtual_size), physical_size);
  RegisterVertexShader(base, header, physical);
  Store(base, header, Load(base, header) | 0x100000);
  result = header;
  return true;
}

bool CreatePixelShader(uint8_t* base, u32 function, const ShaderCalls& calls, u32& result) {
  result = 0;
  if (Load(base, kPixelShaderValidator))
    return false;
  const u32 virtual_size = Load(base, function + 4);
  const u32 physical_size = Load(base, function + 8);
  if (virtual_size > (1u << 20) || physical_size > (1u << 20))
    return false;
  const u32 extra = (Load(base, function) & 1) ? 0x340 : 0;
  const u32 header = calls.xmem_alloc(calls.user, 0x28 + extra + virtual_size, kAttributesObject);
  if (!header)
    return true;
  std::memset(Guest(base, header), 0, 0x28);
  std::memcpy(Guest(base, header + 0x28), Guest(base, function), virtual_size);
  const u32 physical = calls.xmem_alloc(calls.user, physical_size, kAttributesMicrocode);
  if (!physical) {
    calls.xmem_free(calls.user, header, kAttributesObject);
    return true;
  }
  std::memcpy(Guest(base, physical), Guest(base, function + virtual_size), physical_size);
  Store(base, header + 4, 1);
  Store(base, header + 0x14, 0xFFFF0000u);
  Store(base, header + 0x18, physical);
  Store(base, header, 0x100007);  // D3DRTYPE_PIXELSHADER, created by the XDK
  result = header;
  return true;
}

u32 CreateVertexDeclaration(uint8_t* base, u32 elements, const ShaderCalls& calls) {
  // The elements run until one whose stream is 0xFF.
  u32 count = 0;
  while (LoadBE16(Guest(base, elements + count * 12)) != 0xFF) {
    if (++count > 0x1000)
      return 0;
  }
  const u32 size = count * 12 + 0x38;
  const u32 declaration = calls.xmem_alloc(calls.user, size, kAttributesObject);
  if (!declaration)
    return 0;
  u8 used[16] = {};
  u32 last_stream = 0;
  for (u32 i = 0; i < count; ++i) {
    const u32 stream = LoadBE16(Guest(base, elements + i * 12));
    if (stream < 16) {
      used[stream] = 0xFF;
      if (stream > last_stream)
        last_stream = stream;
    }
  }
  u8* out = Guest(base, declaration);
  std::memset(out, 0, size);
  StoreBE32(out + 0x18, count);
  StoreBE32(out + 0x1C, last_stream);
  StoreBE32(out + 4, 1);
  StoreBE32(out + 0, 0x100005);  // D3DRTYPE_VERTEXDECLARATION, created by the XDK
  StoreBE32(out + 0x14, 0xFFFF0000u);
  std::memcpy(out + 0x20, used, 16);  // which streams the elements use, a byte each
  std::memcpy(out + 0x34, Guest(base, elements), count * 12);
  return declaration;
}

namespace {

bool SetShaderConstantsF(Device d, u32 first_register, u32 start, u32 data, u32 count, u32 pending_word,
                         u64 pending_mask) {
  if (!d || !data || start + count > 256 || start + count < start)
    return false;
  std::memcpy(d.bytes + (start + first_register) * 16, Guest(d.base, data), count * 16);
  d.SetPending(pending_word, d.Pending(pending_word) | pending_mask);
  return true;
}

}  // namespace

bool SetVertexShaderConstantsF(Device d, u32 start, u32 data, u32 count, u64 pending_mask) {
  return SetShaderConstantsF(d, 0x78, start, data, count, 0, pending_mask);
}

bool SetPixelShaderConstantsF(Device d, u32 start, u32 data, u32 count, u64 pending_mask) {
  return SetShaderConstantsF(d, 0x178, start, data, count, 1, pending_mask);
}

}  // namespace renut::d3d::port
