#pragma once
// A vertex shader that also hands its position on as an ordinary output.
//
// The recompiled vertex shaders (DXC) declare gl_Position as a variable of its own; the host's geometry
// shaders (rect_list.geom, point_list.geom: glslang) read it from the gl_PerVertex block of gl_in[]. Turnip
// matches the two; Qualcomm's own Adreno driver hands the geometry shader positions that are not the vertex
// shader's, and Showdown Town's full-screen passes (rectangle lists) came out as a screen of smeared streaks.
// With the position also at an ordinary location, the geometry shader reads it there.
#include <cstdint>
#include <vector>

namespace renut::spirv {

// The location the geometry shaders read the position from.
constexpr uint32_t kPositionVaryingLocation = 24;

// Writes into `out` the vertex module `in` with an output at kPositionVaryingLocation that holds what
// gl_Position holds when the entry point returns. False when the module has no gl_Position variable of a
// four-float type, or is not shaped as expected.
inline bool AddPositionVarying(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
  constexpr uint32_t kOpEntryPoint = 15, kOpTypePointer = 32, kOpFunction = 54, kOpFunctionEnd = 56,
                     kOpVariable = 59, kOpLoad = 61, kOpStore = 62, kOpDecorate = 71, kOpReturn = 253;
  constexpr uint32_t kStorageOutput = 3, kDecorationBuiltIn = 11, kDecorationLocation = 30, kBuiltInPosition = 0;
  const size_t words = in.size();
  if (words < 5 || in[0] != 0x07230203u) return false;
  uint32_t bound = in[3];

  // The entry point, the position variable and its pointer type.
  uint32_t entry = 0, position = 0, pointer = 0, pointee = 0;
  size_t entry_at = 0, first_decorate = 0, first_function = 0;
  std::vector<std::pair<uint32_t, uint32_t>> pointers;  // id -> pointee, Output ones
  for (size_t i = 5; i < words;) {
    const uint32_t count = in[i] >> 16, op = in[i] & 0xFFFFu;
    if (!count || i + count > words) return false;
    if (op == kOpFunction) {
      first_function = i;
      break;
    }
    if (op == kOpEntryPoint && count >= 3 && !entry) {
      if (in[i + 1] != 0 /* Vertex */) return false;
      entry = in[i + 2];
      entry_at = i;
    }
    if (op == kOpDecorate && !first_decorate) first_decorate = i;
    if (op == kOpDecorate && count == 4 && in[i + 2] == kDecorationBuiltIn && in[i + 3] == kBuiltInPosition) {
      position = in[i + 1];
    }
    if (op == kOpTypePointer && count == 4 && in[i + 2] == kStorageOutput) pointers.push_back({in[i + 1], in[i + 3]});
    if (op == kOpVariable && count >= 4 && in[i + 2] == position && position) pointer = in[i + 1];
    i += count;
  }
  if (!entry || !position || !pointer || !first_function || !first_decorate) return false;
  for (const auto& [id, type] : pointers) {
    if (id == pointer) pointee = type;
  }
  if (!pointee) return false;  // not an Output variable of a type of its own

  const uint32_t varying = bound++;
  out.clear();
  out.reserve(words + 32);
  out.insert(out.end(), in.begin(), in.begin() + 5);
  for (size_t i = 5; i < words;) {
    const uint32_t count = in[i] >> 16;
    const uint32_t* w = &in[i];
    if (i == entry_at) {
      // The interface gains the new output.
      out.push_back(((count + 1) << 16) | kOpEntryPoint);
      out.insert(out.end(), w + 1, w + count);
      out.push_back(varying);
      i += count;
      continue;
    }
    if (i == first_decorate) out.insert(out.end(), {(4u << 16) | kOpDecorate, varying, kDecorationLocation,
                                                    kPositionVaryingLocation});
    if (i == first_function) {
      // The variable, after every type and global it needs.
      out.insert(out.end(), {(4u << 16) | kOpVariable, pointer, varying, kStorageOutput});
    }
    out.insert(out.end(), w, w + count);
    i += count;
  }
  // In the entry point's function, the position copied before each return.
  std::vector<uint32_t> result;
  result.reserve(out.size() + 32);
  result.insert(result.end(), out.begin(), out.begin() + 5);
  bool in_entry = false;
  uint32_t copies = 0;
  for (size_t i = 5; i < out.size();) {
    const uint32_t count = out[i] >> 16, op = out[i] & 0xFFFFu;
    if (op == kOpFunction) in_entry = out[i + 2] == entry;
    if (op == kOpFunctionEnd) in_entry = false;
    if (in_entry && op == kOpReturn) {
      const uint32_t value = bound++;
      result.insert(result.end(), {(4u << 16) | kOpLoad, pointee, value, position});
      result.insert(result.end(), {(3u << 16) | kOpStore, varying, value});
      ++copies;
    }
    result.insert(result.end(), out.begin() + i, out.begin() + i + count);
    i += count;
  }
  if (!copies) return false;
  result[3] = bound;
  out.swap(result);
  return true;
}

}  // namespace renut::spirv
