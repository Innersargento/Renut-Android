#pragma once
// The key a recompiled shader is found by: XXH3-64 of its microcode with the
// vfetch fields Direct3D rewrites at bind time cleared.
//
// When Direct3D binds a vertex shader to a vertex declaration it patches the
// shader's vfetch instructions in place - fetch constant, format, stride,
// offset, destination swizzle - so the microcode the GPU is handed differs from
// the microcode the game registered (and that was recompiled) in exactly those
// instructions. Clearing everything in a vfetch but its opcode, registers and
// source swizzle gives both the same key. Shared by the runtime
// (plume_gpu.cpp) and tools/shaders/ucode_map.cpp so the two cannot drift.
//
// The microcode is big-endian dwords, as stored in guest memory.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <xxhash.h>

namespace renut::ucode {

inline uint32_t LoadBE(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline void StoreBE(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

// Control-flow instruction `index` (48 bits; two per 96-bit slot).
inline uint64_t ControlFlow(const uint8_t* code, size_t index) {
  const uint8_t* t = code + (index / 2) * 12;
  uint32_t w0 = LoadBE(t), w1 = LoadBE(t + 4), w2 = LoadBE(t + 8);
  if ((index & 1) == 0) return uint64_t(w0) | (uint64_t(w1 & 0xFFFF) << 32);
  return uint64_t((w1 >> 16) | (w2 << 16)) | (uint64_t(w2 >> 16) << 32);
}

inline bool IsExec(uint32_t op) {
  return op == 1 || op == 2 || op == 3 || op == 4 || op == 5 || op == 6 || op == 13 || op == 14;
}

// Instruction slots holding fetch instructions, from the exec blocks'
// sequence bits.
inline std::vector<uint32_t> FetchSlots(const uint8_t* code, size_t size) {
  std::vector<uint32_t> slots;
  const size_t slot_count = size / 12;
  if (slot_count < 1) return slots;
  uint32_t first_exec = 0;
  for (size_t i = 0; i < 32 && i / 2 < slot_count; ++i) {
    uint64_t cf = ControlFlow(code, i);
    if (IsExec(uint32_t(cf >> 44) & 0xF)) {
      first_exec = uint32_t(cf & 0xFFF);
      break;
    }
  }
  for (size_t i = 0; i < size_t(first_exec) * 2 && i / 2 < slot_count; ++i) {
    uint64_t cf = ControlFlow(code, i);
    if (!IsExec(uint32_t(cf >> 44) & 0xF)) continue;
    uint32_t address = uint32_t(cf & 0xFFF), count = uint32_t(cf >> 12) & 7;
    uint32_t sequence = uint32_t(cf >> 16) & 0xFFF;
    for (uint32_t k = 0; k < count; ++k) {
      if ((sequence >> (2 * k)) & 1 && address + k < slot_count) slots.push_back(address + k);
    }
  }
  return slots;
}

inline uint64_t Key(const void* data, size_t size) {
  std::vector<uint8_t> code(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
  for (uint32_t slot : FetchSlots(code.data(), code.size())) {
    uint8_t* instruction = code.data() + size_t(slot) * 12;
    uint32_t w0 = LoadBE(instruction);
    if ((w0 & 0x1F) != 0) continue;  // not a vfetch
    StoreBE(instruction, w0 & 0xC00FFFFF);
    StoreBE(instruction + 4, 0);
    StoreBE(instruction + 8, 0);
  }
  return XXH3_64bits(code.data(), code.size());
}

}  // namespace renut::ucode
