// Takes the interpolant outputs out of a recompiled vertex shader, for draws
// that have no fragment shader.
//
// A recompiled vertex shader declares every export of the guest's shader as an
// Output at its own Location - 24 vec4 of them, the guest's interpolators - and
// writes all of them each vertex. A depth-only draw (the shadow maps, 46% of the
// draws of a town scene) has no fragment shader and nothing reads those outputs,
// but the driver's compiler has no consumer to prune them against: it keeps the
// work that feeds them and the output registers it fills. Without them the
// shader is the position (and the clip distance, which stays: it is a built-in).
//
// This removes, from the module, every Output variable that is not a built-in -
// its declaration, decorations, name, entry point interface entry and the stores
// into it, directly or through an access chain. What computed the stored values
// is left for the compiler's dead code elimination. A module that reads one of
// those outputs back is left alone (empty result).
#pragma once

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace renut {

inline std::vector<uint32_t> StripVertexOutputs(const std::vector<uint32_t>& in, uint32_t* removed = nullptr) {
  constexpr uint32_t kMagic = 0x07230203;
  constexpr uint32_t kOpName = 5, kOpEntryPoint = 15, kOpFunctionCall = 57, kOpVariable = 59, kOpLoad = 61;
  constexpr uint32_t kOpStore = 62, kOpCopyMemory = 63, kOpAccessChain = 65, kOpInBoundsAccessChain = 66;
  constexpr uint32_t kOpDecorate = 71;
  constexpr uint32_t kBuiltIn = 11, kOutput = 3, kVertexStage = 0;
  if (removed) *removed = 0;
  if (in.size() < 6 || in[0] != kMagic) return {};

  std::unordered_set<uint32_t> built_in, outputs;
  bool vertex = false;
  for (size_t i = 5; i < in.size();) {
    const uint32_t words = in[i] >> 16, op = in[i] & 0xFFFF;
    if (!words || i + words > in.size()) return {};
    if (op == kOpEntryPoint && words >= 4 && in[i + 1] == kVertexStage) vertex = true;
    if (op == kOpDecorate && words >= 4 && in[i + 2] == kBuiltIn) built_in.insert(in[i + 1]);
    if (op == kOpVariable && words >= 4 && in[i + 3] == kOutput && !built_in.count(in[i + 2])) {
      outputs.insert(in[i + 2]);
    }
    i += words;
  }
  if (!vertex || outputs.empty()) return {};

  // The pointers into the removed outputs: the variables and the access chains
  // that start at one of them.
  std::unordered_set<uint32_t> pointers = outputs;
  for (size_t i = 5; i < in.size();) {
    const uint32_t words = in[i] >> 16, op = in[i] & 0xFFFF;
    if ((op == kOpAccessChain || op == kOpInBoundsAccessChain) && words >= 5 && pointers.count(in[i + 3])) {
      pointers.insert(in[i + 2]);
    }
    // Anything that reads an output back needs the output.
    if (op == kOpLoad && words >= 4 && pointers.count(in[i + 3])) return {};
    if (op == kOpCopyMemory && words >= 3 && (pointers.count(in[i + 1]) || pointers.count(in[i + 2]))) return {};
    if (op == kOpFunctionCall) {
      for (uint32_t w = 4; w < words; ++w) {
        if (pointers.count(in[i + w])) return {};
      }
    }
    i += words;
  }

  std::vector<uint32_t> out;
  out.reserve(in.size());
  out.insert(out.end(), in.begin(), in.begin() + 5);
  for (size_t i = 5; i < in.size();) {
    const uint32_t words = in[i] >> 16, op = in[i] & 0xFFFF;
    bool keep = true;
    switch (op) {
      case kOpName:
      case kOpDecorate:
        keep = !(words >= 2 && outputs.count(in[i + 1]));
        break;
      case kOpVariable:
        keep = !(words >= 3 && outputs.count(in[i + 2]));
        break;
      case kOpAccessChain:
      case kOpInBoundsAccessChain:
        keep = !(words >= 5 && pointers.count(in[i + 2]));
        break;
      case kOpStore:
        keep = !(words >= 3 && pointers.count(in[i + 1]));
        break;
      case kOpEntryPoint: {
        // Execution model, id, name (a nul-terminated string in whole words),
        // then the interface.
        size_t at = i + 3;
        bool ended = false;
        while (at < i + words && !ended) {
          const uint32_t w = in[at++];
          ended = !(w & 0xFF) || !(w & 0xFF00) || !(w & 0xFF0000) || !(w & 0xFF000000);
        }
        if (!ended) return {};
        std::vector<uint32_t> head(in.begin() + i, in.begin() + at);
        for (size_t w = at; w < i + words; ++w) {
          if (!outputs.count(in[w])) head.push_back(in[w]);
        }
        head[0] = (uint32_t(head.size()) << 16) | kOpEntryPoint;
        out.insert(out.end(), head.begin(), head.end());
        keep = false;
        break;
      }
      default: break;
    }
    if (keep) out.insert(out.end(), in.begin() + i, in.begin() + i + words);
    i += words;
  }
  if (removed) *removed = uint32_t(outputs.size());
  return out;
}

}  // namespace renut
