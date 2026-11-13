// The recompiled shaders' descriptor sets, rewritten for a device whose limits do not take the bindless
// heaps (the Mali-G52: four bound sets, 256 sampled images and 128 samplers a stage, no descriptor
// indexing). See GpuBackend::DrawTextureSet for the host's half.
//
// As compiled, a recompiled shader reads its textures from four unbounded arrays - 2D, 3D, cube and
// samplers, binding 0 of sets 0 to 3 - at the heap indices the draw writes into its shared block, and its
// constants from set 4. Rewritten:
//
//   - the four arrays are 32 entries each (one a fetch slot: RENUT_TEXTURE_SLOT_COUNT), bindings 0 to 3 of
//     set 0, which the host fills per draw with the draw's own textures in slot order, and writes the slot
//     numbers 0..31 where the heap indices were;
//   - set 4 becomes set 1;
//   - the descriptor-indexing capabilities, extension and NonUniform decorations go: the index is a slot
//     number read from a uniform buffer, uniform over the draw.
//
// Only a module that has the heaps or set 4 is touched (the host's own compute and geometry shaders have
// neither).
#pragma once

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace renut::spirv {

inline constexpr uint32_t kPerDrawTextureSlots = 32;
// The sets of the rewritten modules: the draw's textures, then the constants (set 4 as compiled).
inline constexpr uint32_t kPerDrawTextureSet = 0, kPerDrawConstantSet = 1, kCompiledConstantSet = 4;

inline bool RewritePerDrawTextures(const uint32_t* in, size_t words, std::vector<uint32_t>& out) {
  constexpr uint32_t kMagic = 0x07230203;
  constexpr uint32_t kOpExtension = 10, kOpCapability = 17, kOpTypeInt = 21, kOpTypeImage = 25,
                     kOpTypeSampler = 26, kOpTypeSampledImage = 27, kOpTypeArray = 28, kOpTypeRuntimeArray = 29,
                     kOpTypePointer = 32, kOpConstant = 43, kOpVariable = 59, kOpDecorate = 71;
  constexpr uint32_t kDecorationBinding = 33, kDecorationDescriptorSet = 34, kDecorationNonUniform = 5300;
  if (!in || words < 6 || in[0] != kMagic) return false;

  // What the module declares: the image and sampler types, the unbounded arrays of them, the pointers and
  // the variables, and the set of each variable.
  std::unordered_set<uint32_t> resource_types, heap_arrays;
  std::unordered_map<uint32_t, uint32_t> pointee, variable_type, variable_set;
  uint32_t uint_type = 0;
  size_t uint_at = 0;
  for (size_t i = 5; i < words;) {
    const uint32_t count = in[i] >> 16, op = in[i] & 0xFFFF;
    if (!count || i + count > words) return false;
    if ((op == kOpTypeImage || op == kOpTypeSampler || op == kOpTypeSampledImage) && count >= 2) {
      resource_types.insert(in[i + 1]);
    } else if (op == kOpTypeRuntimeArray && count == 3 && resource_types.count(in[i + 2])) {
      heap_arrays.insert(in[i + 1]);
    } else if (op == kOpTypePointer && count == 4) {
      pointee[in[i + 1]] = in[i + 3];
    } else if (op == kOpVariable && count >= 4) {
      variable_type[in[i + 2]] = in[i + 1];
    } else if (op == kOpDecorate && count == 4 && in[i + 2] == kDecorationDescriptorSet) {
      variable_set[in[i + 1]] = in[i + 3];
    } else if (op == kOpTypeInt && count == 4 && in[i + 2] == 32 && in[i + 3] == 0 && !uint_type) {
      uint_type = in[i + 1];
      uint_at = i;
    }
    i += count;
  }
  // The heaps (sets 0-3, whose type is an unbounded array of images or samplers) and the constants (set 4).
  std::unordered_map<uint32_t, uint32_t> heap_binding;  // variable -> its binding in set 0
  bool constants = false;
  for (const auto& [variable, set] : variable_set) {
    if (set == kCompiledConstantSet) {
      constants = true;
      continue;
    }
    const auto type = variable_type.find(variable);
    if (type == variable_type.end()) continue;
    const auto array = pointee.find(type->second);
    if (array == pointee.end() || !heap_arrays.count(array->second)) continue;
    if (set > 3) return false;  // a heap somewhere this rewrite does not know
    heap_binding[variable] = set;
  }
  if (heap_binding.empty() && !constants) return false;

  uint32_t bound = in[3];
  const uint32_t length = bound++;
  const uint32_t new_uint = uint_type ? 0 : bound++;
  if (!uint_type) uint_type = new_uint;
  out.clear();
  out.reserve(words + 16);
  out.insert(out.end(), in, in + 5);
  bool length_declared = false;
  for (size_t i = 5; i < words;) {
    const uint32_t count = in[i] >> 16, op = in[i] & 0xFFFF;
    const uint32_t* w = in + i;
    i += count;
    if (op == kOpCapability && count == 2 && w[1] >= 5301 && w[1] <= 5312) continue;  // descriptor indexing
    if (op == kOpExtension && count >= 2) {
      const char* name = reinterpret_cast<const char*>(w + 1);
      if (std::strncmp(name, "SPV_EXT_descriptor_indexing", (count - 1) * 4) == 0) continue;
    }
    if (op == kOpDecorate && count == 3 && w[2] == kDecorationNonUniform) continue;
    if (op == kOpDecorate && count == 4 && (w[2] == kDecorationDescriptorSet || w[2] == kDecorationBinding)) {
      const auto heap = heap_binding.find(w[1]);
      const bool constant = w[2] == kDecorationDescriptorSet && w[3] == kCompiledConstantSet;
      if (heap != heap_binding.end() || constant) {
        uint32_t value = w[3];
        if (heap != heap_binding.end()) {
          value = w[2] == kDecorationDescriptorSet ? kPerDrawTextureSet : heap->second;
        } else {
          value = kPerDrawConstantSet;
        }
        out.insert(out.end(), {w[0], w[1], w[2], value});
        continue;
      }
    }
    // The module's own 32-bit unsigned type, declared after the first heap array: declared there instead.
    if (op == kOpTypeInt && length_declared && uint_at == size_t(w - in)) continue;
    if (op == kOpTypeRuntimeArray && count == 3 && heap_arrays.count(w[1])) {
      if (!length_declared) {
        if (new_uint || uint_at > size_t(w - in)) out.insert(out.end(), {(4u << 16) | kOpTypeInt, uint_type, 32, 0});
        out.insert(out.end(), {(4u << 16) | kOpConstant, uint_type, length, kPerDrawTextureSlots});
        length_declared = true;
      }
      out.insert(out.end(), {(4u << 16) | kOpTypeArray, w[1], w[2], length});
      continue;
    }
    out.insert(out.end(), w, w + count);
  }
  out[3] = bound;
  return true;
}

}  // namespace renut::spirv
