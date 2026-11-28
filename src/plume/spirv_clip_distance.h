#pragma once
// A shader module without its gl_ClipDistance output.
//
// Every recompiled vertex shader declares the ClipDistance capability and writes gl_ClipDistance[0]
// (renutUserClipDistance: the guest's user clip planes, a huge positive distance when none is on). A device
// without the shaderClipDistance feature may not be handed such a module, and ARM's driver on the Mali-G68
// (r38p1, which has no clip distances) takes it anyway and garbles the vertex position: the title's canyon came
// out as huge flat polygons over the whole screen, depth test or not, while the same mesh drawn by a vertex
// shader without the output was where it belongs. There the output goes - its variable, decorations, name,
// entry point interface entry, the access chains into it and the stores - with the capability. The user clip
// planes are then not applied (nothing is clipped by them).
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace renut::spirv {

// Writes into `out` the module `in` without its ClipDistance output and capability. False when it has neither,
// or reads the output back (the caller keeps the module as it is).
inline bool StripClipDistance(const uint32_t* in, size_t words, std::vector<uint32_t>& out) {
  constexpr uint32_t kOpName = 5, kOpEntryPoint = 15, kOpCapability = 17, kOpFunctionCall = 57, kOpVariable = 59,
                     kOpLoad = 61, kOpStore = 62, kOpCopyMemory = 63, kOpAccessChain = 65,
                     kOpInBoundsAccessChain = 66, kOpDecorate = 71;
  constexpr uint32_t kDecorationBuiltIn = 11, kBuiltInClipDistance = 3, kStorageOutput = 3,
                     kCapabilityClipDistance = 32;
  if (words < 5 || in[0] != 0x07230203u) return false;

  std::unordered_set<uint32_t> clip;  // ids decorated BuiltIn ClipDistance
  bool capability = false;
  for (size_t i = 5; i < words;) {
    const uint32_t count = in[i] >> 16, op = in[i] & 0xFFFFu;
    if (!count || i + count > words) return false;
    if (op == kOpCapability && count == 2 && in[i + 1] == kCapabilityClipDistance) capability = true;
    if (op == kOpDecorate && count >= 4 && in[i + 2] == kDecorationBuiltIn && in[i + 3] == kBuiltInClipDistance) {
      clip.insert(in[i + 1]);
    }
    i += count;
  }
  if (!capability && clip.empty()) return false;

  // The output variables among them (an input one, a geometry stage's gl_in[].gl_ClipDistance, is not handled).
  std::unordered_set<uint32_t> outputs;
  for (size_t i = 5; i < words;) {
    const uint32_t count = in[i] >> 16, op = in[i] & 0xFFFFu;
    if (op == kOpVariable && count >= 4 && clip.count(in[i + 2])) {
      if (in[i + 3] != kStorageOutput) return false;
      outputs.insert(in[i + 2]);
    }
    i += count;
  }
  if (outputs.size() != clip.size()) return false;  // decorated on a block member or something else

  // The pointers into them; anything that reads one back keeps the module as it is.
  std::unordered_set<uint32_t> pointers = outputs;
  for (size_t i = 5; i < words;) {
    const uint32_t count = in[i] >> 16, op = in[i] & 0xFFFFu;
    if ((op == kOpAccessChain || op == kOpInBoundsAccessChain) && count >= 4 && pointers.count(in[i + 3])) {
      pointers.insert(in[i + 2]);
    }
    if (op == kOpLoad && count >= 4 && pointers.count(in[i + 3])) return false;
    if (op == kOpCopyMemory && count >= 3 && (pointers.count(in[i + 1]) || pointers.count(in[i + 2]))) return false;
    if (op == kOpFunctionCall) {
      for (uint32_t w = 4; w < count; ++w) {
        if (pointers.count(in[i + w])) return false;
      }
    }
    i += count;
  }

  out.clear();
  out.reserve(words);
  out.insert(out.end(), in, in + 5);
  for (size_t i = 5; i < words;) {
    const uint32_t count = in[i] >> 16, op = in[i] & 0xFFFFu;
    bool keep = true;
    switch (op) {
      case kOpCapability: keep = !(count == 2 && in[i + 1] == kCapabilityClipDistance); break;
      case kOpName:
      case kOpDecorate: keep = !(count >= 2 && outputs.count(in[i + 1])); break;
      case kOpVariable: keep = !(count >= 3 && outputs.count(in[i + 2])); break;
      case kOpAccessChain:
      case kOpInBoundsAccessChain: keep = !(count >= 4 && pointers.count(in[i + 2])); break;
      case kOpStore: keep = !(count >= 3 && pointers.count(in[i + 1])); break;
      case kOpEntryPoint: {
        // Execution model, id, name (a nul-terminated string in whole words), then the interface.
        size_t at = i + 3;
        bool ended = false;
        while (at < i + count && !ended) {
          const uint32_t w = in[at++];
          ended = !(w & 0xFF) || !(w & 0xFF00) || !(w & 0xFF0000) || !(w & 0xFF000000);
        }
        if (!ended) return false;
        const size_t head_at = out.size();
        out.insert(out.end(), in + i, in + at);
        for (size_t w = at; w < i + count; ++w) {
          if (!outputs.count(in[w])) out.push_back(in[w]);
        }
        out[head_at] = (uint32_t(out.size() - head_at) << 16) | kOpEntryPoint;
        keep = false;
        break;
      }
      default: break;
    }
    if (keep) out.insert(out.end(), in + i, in + i + count);
    i += count;
  }
  return true;
}

}  // namespace renut::spirv
