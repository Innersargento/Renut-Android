#pragma once
// A vertex shader's inputs retyped to the integer attributes they are fed.
//
// The recompiled vertex shaders take every input as a float vector. A driver that converts an integer
// attribute for them (the *_USCALED / *_SSCALED vertex formats, on Turnip) or hands its bits over unconverted
// (what the 32-bit integer inputs rely on, read with asint()) draws them as they are. Qualcomm's own Adreno
// driver has no scaled vertex formats and refuses to compile a vertex shader whose float input is bound to an
// integer attribute ("Shader compilation failed for shaderType: 0"): there the input is declared as the
// integer vector the attribute is, and the conversion is an instruction of the shader.
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace renut::spirv {

// What becomes of the input at a location.
enum VertexInputKind : uint8_t {
  kInputFloat = 0,       // left alone
  kInputSignedNumber,    // int vector, OpConvertSToF: what an SSCALED attribute gave
  kInputUnsignedNumber,  // uint vector, OpConvertUToF: what a USCALED attribute gave
  kInputSignedBits,      // int vector, OpBitcast: the shader reads the bits itself (asint)
  kInputUnsignedBits,    // uint vector, OpBitcast
};

// Whether `code` is a module with a vertex entry point.
inline bool IsVertexModule(const uint32_t* code, size_t words) {
  if (words < 5 || code[0] != 0x07230203u) return false;
  for (size_t i = 5; i < words;) {
    const uint32_t count = code[i] >> 16, op = code[i] & 0xFFFFu;
    if (!count || i + count > words) return false;
    if (op == 15 /* OpEntryPoint */) return count >= 2 && code[i + 1] == 0 /* Vertex */;
    if (op == 54 /* OpFunction */) return false;
    i += count;
  }
  return false;
}

// Writes into `out` the module `in` with the float inputs at the locations of `kinds` (32 of them) retyped.
// False when there is nothing to retype or the module uses an input in a way this does not follow (the caller
// keeps the module as it is).
inline bool RetypeVertexInputs(const std::vector<uint32_t>& in, const uint8_t* kinds, std::vector<uint32_t>& out) {
  constexpr uint32_t kOpTypeInt = 21, kOpTypeFloat = 22, kOpTypeVector = 23, kOpTypePointer = 32, kOpFunction = 54,
                     kOpFunctionCall = 57, kOpVariable = 59, kOpLoad = 61, kOpStore = 62, kOpCopyMemory = 63,
                     kOpAccessChain = 65, kOpInBoundsAccessChain = 66, kOpDecorate = 71, kOpCopyObject = 83,
                     kOpConvertSToF = 111, kOpConvertUToF = 112, kOpBitcast = 124;
  constexpr uint32_t kStorageInput = 1, kDecorationLocation = 30;
  const size_t words = in.size();
  if (words < 5 || in[0] != 0x07230203u) return false;
  uint32_t bound = in[3];

  struct Vector {
    uint32_t id, component, count;
  };
  struct Pointer {
    uint32_t storage, type;
  };
  struct Variable {
    uint32_t id, pointer;
    size_t at;
  };
  std::unordered_map<uint32_t, uint32_t> location;  // id -> Location
  std::unordered_map<uint32_t, Pointer> pointers;
  std::vector<Vector> vectors;
  std::vector<Variable> variables;
  uint32_t int_type[2] = {0, 0};  // [signed]
  uint32_t float_type = 0;
  size_t first_function = 0;
  for (size_t i = 5; i < words;) {
    const uint32_t count = in[i] >> 16, op = in[i] & 0xFFFFu;
    if (!count || i + count > words) return false;
    if (op == kOpFunction) {
      first_function = i;
      break;
    }
    if (op == kOpDecorate && count == 4 && in[i + 2] == kDecorationLocation) location[in[i + 1]] = in[i + 3];
    if (op == kOpTypeInt && count == 4 && in[i + 2] == 32) int_type[in[i + 3] ? 1 : 0] = in[i + 1];
    if (op == kOpTypeFloat && count >= 3 && in[i + 2] == 32) float_type = in[i + 1];
    if (op == kOpTypeVector && count == 4) vectors.push_back({in[i + 1], in[i + 2], in[i + 3]});
    if (op == kOpTypePointer && count == 4) pointers[in[i + 1]] = {in[i + 2], in[i + 3]};
    if (op == kOpVariable && count == 4 && in[i + 3] == kStorageInput) variables.push_back({in[i + 2], in[i + 1], i});
    i += count;
  }
  if (!first_function || !float_type) return false;

  // An input being retyped, or a pointer into one.
  struct Typed {
    bool is_signed, bits;
    uint32_t components;  // 1: a scalar
  };
  std::unordered_map<uint32_t, Typed> typed;
  std::unordered_map<size_t, Typed> retyped_at;  // by the OpVariable's place
  for (const Variable& v : variables) {
    const auto where = location.find(v.id);
    if (where == location.end() || where->second >= 32 || kinds[where->second] == kInputFloat) continue;
    const auto pointer = pointers.find(v.pointer);
    if (pointer == pointers.end() || pointer->second.storage != kStorageInput) continue;
    uint32_t components = 0;
    if (pointer->second.type == float_type) {
      components = 1;
    } else {
      for (const Vector& vector : vectors) {
        if (vector.id == pointer->second.type && vector.component == float_type) components = vector.count;
      }
    }
    if (!components) continue;  // not a float input: it is what its attribute is already
    const uint8_t kind = kinds[where->second];
    const Typed t{kind == kInputSignedNumber || kind == kInputSignedBits,
                  kind == kInputSignedBits || kind == kInputUnsignedBits, components};
    typed[v.id] = t;
    retyped_at[v.at] = t;
  }
  if (typed.empty()) return false;

  out.clear();
  out.reserve(words + 64 + typed.size() * 8);
  out.insert(out.end(), in.begin(), in.begin() + 5);
  // The declarations, without the variables that change.
  for (size_t i = 5; i < first_function;) {
    const uint32_t count = in[i] >> 16;
    if (!retyped_at.count(i)) out.insert(out.end(), in.begin() + i, in.begin() + i + count);
    i += count;
  }
  // The integer types they need (those the module has are used: a scalar or vector type is declared once),
  // the pointers to them, and the variables again, at the end of the declarations.
  auto scalar = [&](bool is_signed) {
    uint32_t& id = int_type[is_signed ? 1 : 0];
    if (!id) {
      id = bound++;
      out.insert(out.end(), {(4u << 16) | kOpTypeInt, id, 32u, is_signed ? 1u : 0u});
    }
    return id;
  };
  auto value_type = [&](bool is_signed, uint32_t components) {
    const uint32_t component = scalar(is_signed);
    if (components == 1) return component;
    for (const Vector& vector : vectors) {
      if (vector.component == component && vector.count == components) return vector.id;
    }
    const uint32_t id = bound++;
    out.insert(out.end(), {(4u << 16) | kOpTypeVector, id, component, components});
    vectors.push_back({id, component, components});
    return id;
  };
  uint32_t pointer_type[2][5] = {};
  auto pointer_to = [&](bool is_signed, uint32_t components) {
    uint32_t& id = pointer_type[is_signed ? 1 : 0][components];
    if (!id) {
      const uint32_t pointee = value_type(is_signed, components);
      id = bound++;
      out.insert(out.end(), {(4u << 16) | kOpTypePointer, id, kStorageInput, pointee});
    }
    return id;
  };
  for (const Variable& v : variables) {
    const auto found = retyped_at.find(v.at);
    if (found == retyped_at.end()) continue;
    const Typed& t = found->second;
    if (t.components > 4) return false;
    const uint32_t pointer = pointer_to(t.is_signed, t.components);
    pointer_to(t.is_signed, 1);  // for an access chain into it
    out.insert(out.end(), {(4u << 16) | kOpVariable, pointer, v.id, kStorageInput});
  }

  // The functions: a load of a retyped input is a load of the integers and their conversion.
  for (size_t i = first_function; i < words;) {
    const uint32_t count = in[i] >> 16, op = in[i] & 0xFFFFu;
    if (!count || i + count > words) return false;
    const uint32_t* w = &in[i];
    if (op == kOpLoad && count >= 4) {
      const auto found = typed.find(w[3]);
      if (found != typed.end()) {
        const Typed& t = found->second;
        const uint32_t loaded = bound++;
        const size_t at = out.size();
        out.insert(out.end(), w, w + count);
        out[at + 1] = value_type(t.is_signed, t.components);
        out[at + 2] = loaded;
        out.insert(out.end(), {(4u << 16) | (t.bits ? kOpBitcast : t.is_signed ? kOpConvertSToF : kOpConvertUToF),
                               w[1], w[2], loaded});
        i += count;
        continue;
      }
    } else if ((op == kOpAccessChain || op == kOpInBoundsAccessChain) && count >= 4) {
      const auto found = typed.find(w[3]);
      if (found != typed.end()) {
        const Typed t = found->second;
        if (count != 5 || t.components == 1) return false;
        const size_t at = out.size();
        out.insert(out.end(), w, w + count);
        out[at + 1] = pointer_type[t.is_signed ? 1 : 0][1];
        typed[w[2]] = {t.is_signed, t.bits, 1};
        i += count;
        continue;
      }
    } else if (op == kOpStore || op == kOpCopyMemory || op == kOpCopyObject || op == kOpFunctionCall) {
      // An input used as anything else is not followed.
      // (a store's and a copy's operands after the two ids are literals: "Aligned 4" is not input 4)
      const bool two = op == kOpStore || op == kOpCopyMemory;
      const uint32_t first = two ? 1 : op == kOpCopyObject ? 3 : 4;
      const uint32_t last = two ? std::min<uint32_t>(count, 3) : count;
      for (uint32_t k = first; k < last; ++k) {
        if (typed.count(w[k])) return false;
      }
    }
    out.insert(out.end(), w, w + count);
    i += count;
  }
  out[3] = bound;
  return true;
}

}  // namespace renut::spirv
