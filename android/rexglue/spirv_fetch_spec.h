// Turns the live vertex fetch table reads of a recompiled vertex shader into
// specialization constants.
//
// A vfetch the shader indexes with a computed register (vfetchLive) reads what
// to fetch - stream, format, stride, offset, exponent scale - from a table in
// the shared uniform block, and the recompiled code switches over the ~40 data
// formats on a value that the driver's compiler cannot see. Every fetch site
// is then a chain: load the table entry, branch on its format, load the data
// through a device address, decode. The sites cannot overlap, and a skinned
// mesh pays for each of them once a draw (the vertex shaders with live
// fetches cost 25-34 us a draw against 11-15 us for the ones without).
//
// The table is not data of the draw, though: it comes from the shader's own
// vfetch instructions (HostShader::live_fetches) and is the same every time the
// shader runs. So its entries can be the pipeline's specialization constants,
// which the compiler folds - the switch collapses to the one format that is
// used and the loads of the sites no longer wait for one another.
//
// This rewrites a module, once per shader, so that every
//   OpLoad %v4uint (OpAccessChain %table 0 <constant first + slot>)
// becomes an OpCompositeConstruct of four OpSpecConstants with ids
// spec_base + slot * 4 + word. The host then lists the table's words as the
// pipeline's constants (zero, the default, is not listed). The module still
// reads the table everywhere else, so the uniform buffer keeps being written.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace renut {

// The table is the uniform block at (set, binding); `first` is the index of its
// first entry in that block's array of uint4 and `count` the number of entries.
// Returns an empty vector when nothing was rewritten.
inline std::vector<uint32_t> SpecializeFetchTable(const std::vector<uint32_t>& in, uint32_t set, uint32_t binding,
                                                  uint32_t first, uint32_t count, uint32_t spec_base,
                                                  uint32_t* replaced = nullptr) {
  constexpr uint32_t kMagic = 0x07230203;
  constexpr uint32_t kOpTypeVoid = 19, kOpTypeForwardPointer = 39, kOpTypeInt = 21, kOpTypeVector = 23;
  constexpr uint32_t kOpConstant = 43, kOpSpecConstant = 50, kOpFunction = 54, kOpVariable = 59, kOpLoad = 61;
  constexpr uint32_t kOpAccessChain = 65, kOpDecorate = 71, kOpCompositeConstruct = 80;
  constexpr uint32_t kSpecId = 1, kBinding = 33, kDescriptorSet = 34, kUniform = 2;
  if (replaced) *replaced = 0;
  if (in.size() < 6 || in[0] != kMagic) return {};

  std::unordered_map<uint32_t, uint32_t> set_of, binding_of, constant_of;
  std::unordered_map<uint32_t, uint32_t> chain_slot;  // access chain result -> table slot
  struct Load {
    size_t at;
    uint32_t slot;
  };
  std::vector<Load> loads;
  uint32_t uint_type = 0, v4uint_type = 0, table = 0;
  size_t first_type = 0, first_function = 0;

  for (size_t i = 5; i < in.size();) {
    const uint32_t words = in[i] >> 16, op = in[i] & 0xFFFF;
    if (!words || i + words > in.size()) return {};
    if (!first_type && op >= kOpTypeVoid && op <= kOpTypeForwardPointer) first_type = i;
    switch (op) {
      case kOpDecorate:
        if (words >= 4) {
          if (in[i + 2] == kDescriptorSet) set_of[in[i + 1]] = in[i + 3];
          if (in[i + 2] == kBinding) binding_of[in[i + 1]] = in[i + 3];
        }
        break;
      case kOpTypeInt:
        if (words == 4 && in[i + 2] == 32 && in[i + 3] == 0) uint_type = in[i + 1];
        break;
      case kOpTypeVector:
        if (words == 4 && uint_type && in[i + 2] == uint_type && in[i + 3] == 4) v4uint_type = in[i + 1];
        break;
      case kOpConstant:
        if (words == 4) constant_of[in[i + 2]] = in[i + 3];
        break;
      case kOpVariable:
        if (words >= 4 && in[i + 3] == kUniform) {
          auto s = set_of.find(in[i + 2]);
          auto b = binding_of.find(in[i + 2]);
          if (s != set_of.end() && b != binding_of.end() && s->second == set && b->second == binding) {
            table = in[i + 2];
          }
        }
        break;
      case kOpAccessChain:
        if (table && words == 6 && in[i + 3] == table) {
          auto zero = constant_of.find(in[i + 4]);
          auto index = constant_of.find(in[i + 5]);
          if (zero != constant_of.end() && zero->second == 0 && index != constant_of.end() &&
              index->second >= first && index->second < first + count) {
            chain_slot[in[i + 2]] = index->second - first;
          }
        }
        break;
      case kOpLoad:
        if (words >= 4 && v4uint_type && in[i + 1] == v4uint_type) {
          auto chain = chain_slot.find(in[i + 3]);
          if (chain != chain_slot.end()) loads.push_back({i, chain->second});
        }
        break;
      case kOpFunction:
        if (!first_function) first_function = i;
        break;
      default: break;
    }
    i += words;
  }
  if (loads.empty() || !first_type || !first_function || first_type > first_function) return {};

  // One id per word of every slot that is read.
  const uint32_t bound = in[3];
  std::vector<uint32_t> id_of(size_t(count) * 4, 0);
  uint32_t next = bound;
  for (const Load& load : loads) {
    for (uint32_t w = 0; w < 4; ++w) {
      if (!id_of[load.slot * 4 + w]) id_of[load.slot * 4 + w] = next++;
    }
  }

  std::vector<uint32_t> out;
  out.reserve(in.size() + (next - bound) * 8 + loads.size() * 3);
  out.insert(out.end(), in.begin(), in.begin() + 5);
  out[3] = next;
  size_t load_index = 0;
  for (size_t i = 5; i < in.size();) {
    const uint32_t words = in[i] >> 16;
    if (i == first_type) {
      for (uint32_t s = 0; s < count * 4; ++s) {
        if (!id_of[s]) continue;
        out.insert(out.end(), {(4u << 16) | kOpDecorate, id_of[s], kSpecId, spec_base + s});
      }
    }
    if (i == first_function) {
      for (uint32_t s = 0; s < count * 4; ++s) {
        if (!id_of[s]) continue;
        out.insert(out.end(), {(4u << 16) | kOpSpecConstant, uint_type, id_of[s], 0u});
      }
    }
    if (load_index < loads.size() && loads[load_index].at == i) {
      const uint32_t slot = loads[load_index].slot;
      out.insert(out.end(), {(7u << 16) | kOpCompositeConstruct, v4uint_type, in[i + 2], id_of[slot * 4],
                             id_of[slot * 4 + 1], id_of[slot * 4 + 2], id_of[slot * 4 + 3]});
      ++load_index;
    } else {
      out.insert(out.end(), in.begin() + i, in.begin() + i + words);
    }
    i += words;
  }
  if (replaced) *replaced = uint32_t(loads.size());
  return out;
}

}  // namespace renut
