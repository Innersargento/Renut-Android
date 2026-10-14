// Turns the boolean register specialization constants of a recompiled shader
// back into reads of the shared uniform block.
//
// The recompiled shaders test Xenos boolean registers through specialization
// constants (RENUT_SPEC_BOOLS_BASE + word), so the driver drops the branches a
// draw does not take. The price is paid in pipelines: every combination of
// bits a pair of shaders sees is a program the driver compiles from scratch.
// The lit material tests sixteen of them (eight chained point lights and the
// shadow cascades), and a walk through Showdown Town asked for 9,769 programs
// that are 2,715 once the booleans are left out - each one 50-100 ms on a
// worker, and a piece of the scene missing until it is done. Specializing them
// was measured to buy no GPU time (26/09: FS program size is not the per-draw
// cost), so the booleans are read where the host writes them anyway, at
// RENUT_SHARED_BOOLEANS_OFFSET of the hot shared block, every draw.
//
// Each such constant is a uint OpSpecConstant used only inside the one
// function the compiler leaves (every use is inlined into main). Rather than
// rewriting its uses - its ids are small numbers that literals in the code can
// share - its definition moves: the global OpSpecConstant and its SpecId go,
// and the same id is defined again at the top of main as a component of the
// uint4 loaded from the block. A module that does not fit that shape (more than
// one function, a constant used outside main, no hot block) is left alone and
// keeps its specialization.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace renut {

// The block is the uniform buffer at (set, binding), an array of uint4 as its
// only member; the booleans are its uint4s `first` and `first + 1`. Their
// constants have ids spec_base .. spec_base + 7. Returns an empty vector when
// nothing was changed; `moved` gets the number of constants that became loads.
inline std::vector<uint32_t> MakeBooleansDynamic(const std::vector<uint32_t>& in, uint32_t set, uint32_t binding,
                                                 uint32_t first, uint32_t spec_base, uint32_t* moved = nullptr) {
  constexpr uint32_t kMagic = 0x07230203;
  constexpr uint32_t kOpLine = 8, kOpTypeInt = 21, kOpTypeVector = 23, kOpTypeArray = 28;
  constexpr uint32_t kOpTypeStruct = 30, kOpTypePointer = 32, kOpConstant = 43, kOpConstantComposite = 44;
  constexpr uint32_t kOpSpecConstant = 50, kOpSpecConstantComposite = 51, kOpSpecConstantOp = 52;
  constexpr uint32_t kOpFunction = 54, kOpFunctionEnd = 56, kOpVariable = 59;
  constexpr uint32_t kOpLoad = 61, kOpAccessChain = 65, kOpDecorate = 71, kOpCompositeExtract = 81;
  constexpr uint32_t kOpLabel = 248, kOpNoLine = 317;
  constexpr uint32_t kSpecId = 1, kBinding = 33, kDescriptorSet = 34, kUniform = 2;
  if (moved) *moved = 0;
  if (in.size() < 6 || in[0] != kMagic) return {};

  std::unordered_map<uint32_t, uint32_t> set_of, binding_of, word_of;  // word_of: constant id -> boolean word
  std::unordered_map<uint32_t, uint32_t> pointee_of;                    // Uniform pointer type -> pointee
  std::unordered_map<uint32_t, uint32_t> first_member_of, element_of;   // struct -> member 0, array -> element
  std::unordered_map<uint32_t, uint32_t> vector4_of;                    // uint4 type -> its uint type
  std::unordered_set<uint32_t> uint_types;
  struct Constant {
    uint32_t type, value;
  };
  std::unordered_map<uint32_t, Constant> constants;
  uint32_t block = 0, block_type = 0, uint_type = 0, functions = 0;
  size_t first_function = 0;

  // Decorations come first in a module, so a SpecId is known before its constant.
  for (size_t i = 5; i < in.size();) {
    const uint32_t words = in[i] >> 16, op = in[i] & 0xFFFF;
    if (!words || i + words > in.size()) return {};
    if (op == kOpDecorate && words >= 4) {
      if (in[i + 2] == kDescriptorSet) set_of[in[i + 1]] = in[i + 3];
      if (in[i + 2] == kBinding) binding_of[in[i + 1]] = in[i + 3];
      if (in[i + 2] == kSpecId && in[i + 3] >= spec_base && in[i + 3] < spec_base + 8) {
        word_of[in[i + 1]] = in[i + 3] - spec_base;
      }
    }
    i += words;
  }
  if (word_of.empty()) return {};

  bool in_function = false, shape_ok = true;
  auto uses_constant = [&](size_t from, size_t to) {
    for (size_t k = from; k < to; ++k) {
      if (word_of.count(in[k])) return true;
    }
    return false;
  };
  for (size_t i = 5; i < in.size();) {
    const uint32_t words = in[i] >> 16, op = in[i] & 0xFFFF;
    switch (op) {
      case kOpTypeInt:
        if (words == 4 && in[i + 2] == 32 && in[i + 3] == 0) uint_types.insert(in[i + 1]);
        break;
      case kOpTypeVector:
        if (words == 4 && in[i + 3] == 4 && uint_types.count(in[i + 2])) vector4_of[in[i + 1]] = in[i + 2];
        break;
      case kOpTypeArray:
        if (words == 4) element_of[in[i + 1]] = in[i + 2];
        // An array sized by one of the constants could not lose it.
        if (words == 4 && word_of.count(in[i + 3])) shape_ok = false;
        break;
      case kOpTypeStruct:
        if (words >= 3) first_member_of[in[i + 1]] = in[i + 2];
        break;
      case kOpTypePointer:
        if (words == 4 && in[i + 2] == kUniform) pointee_of[in[i + 1]] = in[i + 3];
        break;
      case kOpConstant:
        if (words == 4) constants[in[i + 2]] = {in[i + 1], in[i + 3]};
        break;
      case kOpConstantComposite:
      case kOpSpecConstantComposite:
        if (uses_constant(i + 3, i + words)) shape_ok = false;
        break;
      case kOpSpecConstantOp:
        if (uses_constant(i + 4, i + words)) shape_ok = false;
        break;
      case kOpSpecConstant:
        if (words == 4 && word_of.count(in[i + 2])) {
          if (!uint_types.count(in[i + 1]) || (uint_type && uint_type != in[i + 1])) shape_ok = false;
          uint_type = in[i + 1];
        }
        break;
      case kOpVariable:
        // A variable initialized from one of them, in or out of a function.
        if (words >= 5 && word_of.count(in[i + 4])) shape_ok = false;
        if (!in_function && words >= 4 && in[i + 3] == kUniform) {
          auto s = set_of.find(in[i + 2]);
          auto b = binding_of.find(in[i + 2]);
          if (s != set_of.end() && b != binding_of.end() && s->second == set && b->second == binding) {
            block = in[i + 2];
            block_type = in[i + 1];
          }
        }
        break;
      case kOpFunction:
        if (!first_function) first_function = i;
        ++functions;
        in_function = true;
        break;
      case kOpFunctionEnd:
        in_function = false;
        break;
      default: break;
    }
    i += words;
  }
  if (!shape_ok || functions != 1 || !uint_type || !block || !first_function) return {};

  // The block's uint4: pointer -> struct -> array -> uint4 of the constants' uint.
  auto pointee = pointee_of.find(block_type);
  if (pointee == pointee_of.end()) return {};
  auto member = first_member_of.find(pointee->second);
  if (member == first_member_of.end()) return {};
  auto element = element_of.find(member->second);
  if (element == element_of.end()) return {};
  auto vector = vector4_of.find(element->second);
  if (vector == vector4_of.end() || vector->second != uint_type) return {};
  const uint32_t uint4_type = element->second;

  uint32_t next = in[3];
  std::vector<uint32_t> globals;  // types and constants the loads need, before the function
  uint32_t pointer_type = 0;
  for (const auto& [pointer, type] : pointee_of) {
    if (type == uint4_type) pointer_type = pointer;
  }
  if (!pointer_type) {
    pointer_type = next++;
    globals.insert(globals.end(), {(4u << 16) | kOpTypePointer, pointer_type, kUniform, uint4_type});
  }
  auto constant = [&](uint32_t value) {
    for (const auto& [id, c] : constants) {
      if (c.type == uint_type && c.value == value) return id;
    }
    const uint32_t id = next++;
    globals.insert(globals.end(), {(4u << 16) | kOpConstant, uint_type, id, value});
    constants[id] = {uint_type, value};
    return id;
  };
  const uint32_t zero = constant(0);
  bool half_used[2] = {false, false};
  for (const auto& [id, word] : word_of) half_used[word / 4] = true;
  uint32_t index[2] = {0, 0};
  for (uint32_t h = 0; h < 2; ++h) {
    if (half_used[h]) index[h] = constant(first + h);
  }

  // At the top of main: one load per half of the booleans, then each constant
  // defined again under its own id.
  std::vector<uint32_t> loads;
  uint32_t vector_id[2] = {0, 0};
  for (uint32_t h = 0; h < 2; ++h) {
    if (!half_used[h]) continue;
    const uint32_t chain = next++;
    vector_id[h] = next++;
    loads.insert(loads.end(), {(6u << 16) | kOpAccessChain, pointer_type, chain, block, zero, index[h]});
    loads.insert(loads.end(), {(4u << 16) | kOpLoad, uint4_type, vector_id[h], chain});
  }
  for (const auto& [id, word] : word_of) {
    loads.insert(loads.end(), {(5u << 16) | kOpCompositeExtract, uint_type, id, vector_id[word / 4], word % 4});
  }

  std::vector<uint32_t> out;
  out.reserve(in.size() + globals.size() + loads.size());
  out.insert(out.end(), in.begin(), in.begin() + 5);
  out[3] = next;
  // In main: past OpFunction, its parameters and the entry label, the entry
  // block's variables (which must come first) and any line markers.
  enum { kBefore, kHeader, kEntry, kDone } place = kBefore;
  for (size_t i = 5; i < in.size();) {
    const uint32_t words = in[i] >> 16, op = in[i] & 0xFFFF;
    if (i == first_function) {
      out.insert(out.end(), globals.begin(), globals.end());
      place = kHeader;
    }
    if (place == kEntry && op != kOpVariable && op != kOpLine && op != kOpNoLine) {
      out.insert(out.end(), loads.begin(), loads.end());
      place = kDone;
    }
    const bool drop = (op == kOpDecorate && words >= 4 && in[i + 2] == kSpecId && word_of.count(in[i + 1])) ||
                      (op == kOpSpecConstant && words == 4 && word_of.count(in[i + 2]));
    if (!drop) out.insert(out.end(), in.begin() + i, in.begin() + i + words);
    if (place == kHeader && op == kOpLabel) place = kEntry;
    i += words;
  }
  if (place != kDone) return {};
  if (moved) *moved = uint32_t(word_of.size());
  return out;
}

}  // namespace renut
