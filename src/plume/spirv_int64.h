#pragma once
// A shader module without 64-bit integers.
//
// The recompiled shaders read the guest's constants and streams through buffer device addresses, which they
// make as 64-bit integers: two 32-bit halves joined with a shift and an or, an offset added, compared with
// zero, cast to a pointer. A device without shaderInt64 (Qualcomm's own Adreno driver) compiles none of them:
// "Shader compilation failed", and every pipeline with such a shader is missing. There an address is a vector
// of two 32-bit integers (low, high) instead, which SPIR-V 1.5 casts to a physical pointer as it does a 64-bit
// integer; the handful of operations the shaders use on addresses are rewritten for the pair.
//
// The module becomes SPIR-V 1.5 (the cast needs it), whose entry points name every global variable they use,
// so the interface lists are completed as well.
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace renut::spirv {

// Whether `code` declares a 64-bit integer type.
inline bool UsesInt64(const uint32_t* code, size_t words) {
  if (words < 5 || code[0] != 0x07230203u) return false;
  for (size_t i = 5; i < words;) {
    const uint32_t count = code[i] >> 16, op = code[i] & 0xFFFFu;
    if (!count || i + count > words) return false;
    if (op == 21 /* OpTypeInt */ && count == 4 && code[i + 2] == 64) return true;
    if (op == 54 /* OpFunction */) return false;
    i += count;
  }
  return false;
}

// Writes into `out` the module `in` with its unsigned 64-bit integers as vectors of two 32-bit ones. False when
// the module has none, or uses them in a way this does not follow (the caller keeps the module as it is).
inline bool LowerInt64(const uint32_t* in, size_t words, std::vector<uint32_t>& out) {
  enum : uint32_t {
    kOpUndef = 1, kOpSource = 3, kOpSourceContinued = 2, kOpSourceExtension = 4, kOpName = 5, kOpMemberName = 6,
    kOpString = 7, kOpExtension = 10, kOpExtInstImport = 11, kOpMemoryModel = 14, kOpEntryPoint = 15,
    kOpExecutionMode = 16, kOpCapability = 17, kOpTypeBool = 20, kOpTypeInt = 21, kOpTypeFloat = 22,
    kOpTypeVector = 23, kOpConstant = 43, kOpConstantComposite = 44, kOpConstantNull = 46, kOpSpecConstant = 50,
    kOpFunction = 54, kOpFunctionParameter = 55, kOpFunctionCall = 57, kOpVariable = 59, kOpLoad = 61,
    kOpDecorate = 71, kOpMemberDecorate = 72, kOpCompositeConstruct = 80, kOpCompositeExtract = 81,
    kOpCopyObject = 83, kOpUConvert = 113, kOpConvertPtrToU = 117, kOpConvertUToPtr = 120, kOpBitcast = 124,
    kOpIAdd = 128, kOpAny = 154, kOpAll = 155, kOpSelect = 169, kOpIEqual = 170, kOpINotEqual = 171,
    kOpULessThan = 176, kOpShiftLeftLogical = 196, kOpBitwiseOr = 197, kOpBitwiseXor = 198, kOpBitwiseAnd = 199,
    kOpPhi = 245, kOpModuleProcessed = 330,
  };
  constexpr uint32_t kCapabilityInt64 = 11, kStorageFunction = 7;
  if (words < 5 || in[0] != 0x07230203u) return false;
  uint32_t bound = in[3];

  // The declarations: the types involved, where they are, and the global variables.
  uint32_t wide = 0, uint_type = 0, bool_type = 0, pair = 0, bool_pair = 0;
  size_t wide_at = 0, uint_at = 0, pair_at = 0, first_function = 0;
  std::vector<uint32_t> globals;
  for (size_t i = 5; i < words;) {
    const uint32_t count = in[i] >> 16, op = in[i] & 0xFFFFu;
    if (!count || i + count > words) return false;
    if (op == kOpFunction) {
      first_function = i;
      break;
    }
    if (op == kOpTypeInt && count == 4) {
      if (in[i + 2] == 64) {
        if (in[i + 3] != 0 || wide) return false;  // a signed one, or two
        wide = in[i + 1];
        wide_at = i;
      } else if (in[i + 2] == 32 && in[i + 3] == 0) {
        uint_type = in[i + 1];
        uint_at = i;
      }
    }
    if (op == kOpTypeFloat && count >= 3 && in[i + 2] == 64) return false;
    if (op == kOpTypeBool && count == 2) bool_type = in[i + 1];
    if (op == kOpTypeVector && count == 4 && in[i + 3] == 2) {
      if (uint_type && in[i + 2] == uint_type) {
        pair = in[i + 1];
        pair_at = i;
      }
      if (bool_type && in[i + 2] == bool_type) bool_pair = in[i + 1];
      if (wide && in[i + 2] == wide) return false;
    }
    if (op == kOpTypeVector && count == 4 && wide && in[i + 2] == wide) return false;
    if (op == kOpVariable && count >= 4 && in[i + 3] != kStorageFunction) globals.push_back(in[i + 2]);
    if (op == kOpSpecConstant && wide && in[i + 1] == wide) return false;
    i += count;
  }
  if (!wide || !first_function) return false;
  if (!uint_type || uint_at > wide_at) return false;
  // The pair's type: the module's own vector of two 32-bit integers when it has one (a vector type is declared
  // once), declared where the 64-bit type was if it came later; else a new one under the 64-bit type's id.
  const uint32_t pair_type = pair ? pair : wide;

  out.clear();
  out.reserve(words + words / 4 + 64);
  out.insert(out.end(), in, in + 5);
  out[1] = 0x00010500u;
  auto emit = [&](std::initializer_list<uint32_t> w) { out.insert(out.end(), w); };
  auto instruction = [](uint32_t op, uint32_t count) { return (count << 16) | op; };
  // Words of an instruction that name a type: its result type, and the operands of a type declaration.
  auto literal_first_word = [](uint32_t op) {
    return op == kOpSource || op == kOpSourceContinued || op == kOpSourceExtension || op == kOpExtension ||
           op == kOpMemoryModel || op == kOpEntryPoint || op == kOpCapability || op == kOpModuleProcessed ||
           op == kOpName || op == kOpMemberName || op == kOpString || op == kOpExtInstImport || op == kOpDecorate ||
           op == kOpMemberDecorate || op == kOpExecutionMode;
  };
  auto is_type_declaration = [](uint32_t op) { return op >= 19 && op <= 39; };
  auto copy_retyped = [&](const uint32_t* w, uint32_t count, uint32_t op) {
    const size_t at = out.size();
    out.insert(out.end(), w, w + count);
    if (pair_type == wide) return;
    if (is_type_declaration(op)) {
      for (uint32_t k = 2; k < count; ++k) {
        if (out[at + k] == wide) out[at + k] = pair_type;
      }
    } else if (!literal_first_word(op) && count >= 3 && out[at + 1] == wide) {
      out[at + 1] = pair_type;
    }
  };

  std::unordered_set<uint32_t> wide_ids;            // results of the 64-bit type
  std::unordered_map<uint32_t, uint64_t> constant;  // the ones whose value is known
  uint32_t zero = 0, one = 0;
  auto is_wide = [&](uint32_t id) { return wide_ids.count(id) != 0; };

  for (size_t i = 5; i < words;) {
    const uint32_t count = in[i] >> 16, op = in[i] & 0xFFFFu;
    if (!count || i + count > words) return false;
    const uint32_t* w = &in[i];
    const size_t next = i + count;
    if (i == first_function) {
      // The helpers the functions use, at the end of the declarations.
      zero = bound++;
      one = bound++;
      emit({instruction(kOpConstant, 4), uint_type, zero, 0u});
      emit({instruction(kOpConstant, 4), uint_type, one, 1u});
      if (!bool_type) {
        bool_type = bound++;
        emit({instruction(kOpTypeBool, 2), bool_type});
      }
      if (!bool_pair) {
        bool_pair = bound++;
        emit({instruction(kOpTypeVector, 4), bool_pair, bool_type, 2u});
      }
    }
    if (i < first_function) {
      if (op == kOpCapability && count == 2 && w[1] == kCapabilityInt64) {
        i = next;
        continue;
      }
      if (op == kOpEntryPoint) {
        // Every global variable in the interface (SPIR-V 1.4 and later).
        size_t name_end = 3;
        while (name_end < count && (w[name_end] & 0xFF000000u) != 0) ++name_end;
        ++name_end;  // the word with the terminator
        if (name_end > count) return false;
        std::unordered_set<uint32_t> listed(w + name_end, w + count);
        const size_t at = out.size();
        out.insert(out.end(), w, w + count);
        for (const uint32_t id : globals) {
          if (listed.insert(id).second) out.push_back(id);
        }
        const size_t length = out.size() - at;
        if (length > 0xFFFF) return false;
        out[at] = instruction(kOpEntryPoint, uint32_t(length));
        i = next;
        continue;
      }
      if (i == wide_at) {
        emit({instruction(kOpTypeVector, 4), pair_type, uint_type, 2u});
        i = next;
        continue;
      }
      if (pair && i == pair_at) {
        if (pair_at < wide_at) copy_retyped(w, count, op);  // already where it is needed
        i = next;
        continue;
      }
      if (op == kOpConstant && count == 5 && w[1] == wide) {
        const uint32_t low = bound++, high = bound++;
        emit({instruction(kOpConstant, 4), uint_type, low, w[3]});
        emit({instruction(kOpConstant, 4), uint_type, high, w[4]});
        emit({instruction(kOpConstantComposite, 5), pair_type, w[2], low, high});
        wide_ids.insert(w[2]);
        constant[w[2]] = uint64_t(w[3]) | (uint64_t(w[4]) << 32);
        i = next;
        continue;
      }
      if (!literal_first_word(op) && !is_type_declaration(op) && count >= 3 && w[1] == wide) {
        if (op != kOpUndef && op != kOpConstantNull) return false;
        wide_ids.insert(w[2]);
      }
      copy_retyped(w, count, op);
      i = next;
      continue;
    }

    // The functions.
    const bool wide_result = count >= 3 && w[1] == wide && !literal_first_word(op);
    auto extract = [&](uint32_t value, uint32_t index) {
      const uint32_t id = bound++;
      emit({instruction(kOpCompositeExtract, 5), uint_type, id, value, index});
      return id;
    };
    if (wide_result) {
      wide_ids.insert(w[2]);
      switch (op) {
        case kOpUConvert:
          if (count != 4 || is_wide(w[3])) return false;
          emit({instruction(kOpCompositeConstruct, 5), pair_type, w[2], w[3], zero});
          break;
        case kOpShiftLeftLogical: {
          if (count != 5) return false;
          const auto amount = constant.find(w[4]);
          if (amount == constant.end()) return false;
          if (amount->second == 0) {
            emit({instruction(kOpCopyObject, 4), pair_type, w[2], w[3]});
          } else if (amount->second == 32) {
            const uint32_t low = extract(w[3], 0);
            emit({instruction(kOpCompositeConstruct, 5), pair_type, w[2], zero, low});
          } else {
            return false;
          }
          break;
        }
        case kOpBitwiseOr:
        case kOpBitwiseXor:
        case kOpBitwiseAnd: {
          if (count != 5) return false;
          const auto a = constant.find(w[3]), b = constant.find(w[4]);
          if (a != constant.end() && b != constant.end()) {
            constant[w[2]] = op == kOpBitwiseOr    ? (a->second | b->second)
                             : op == kOpBitwiseXor ? (a->second ^ b->second)
                                                   : (a->second & b->second);
          }
          copy_retyped(w, count, op);  // component by component
          break;
        }
        case kOpIAdd: {
          if (count != 5) return false;
          const uint32_t a_low = extract(w[3], 0), a_high = extract(w[3], 1);
          const uint32_t b_low = extract(w[4], 0), b_high = extract(w[4], 1);
          const uint32_t low = bound++, carried = bound++, carry = bound++, high = bound++, sum = bound++;
          emit({instruction(kOpIAdd, 5), uint_type, low, a_low, b_low});
          emit({instruction(kOpULessThan, 5), bool_type, carried, low, a_low});
          emit({instruction(kOpSelect, 6), uint_type, carry, carried, one, zero});
          emit({instruction(kOpIAdd, 5), uint_type, high, a_high, b_high});
          emit({instruction(kOpIAdd, 5), uint_type, sum, high, carry});
          emit({instruction(kOpCompositeConstruct, 5), pair_type, w[2], low, sum});
          break;
        }
        case kOpConvertPtrToU:
          if (count != 4) return false;
          emit({instruction(kOpBitcast, 4), pair_type, w[2], w[3]});
          break;
        case kOpPhi:
        case kOpSelect:
        case kOpCopyObject:
        case kOpLoad:
        case kOpFunctionParameter:
        case kOpFunctionCall:
        case kOpCompositeExtract:
        case kOpUndef:
          copy_retyped(w, count, op);
          break;
        default:
          return false;
      }
      i = next;
      continue;
    }
    switch (op) {
      case kOpIEqual:
      case kOpINotEqual:
        if (count == 5 && (is_wide(w[3]) || is_wide(w[4]))) {
          const uint32_t each = bound++;
          emit({instruction(op, 5), bool_pair, each, w[3], w[4]});
          emit({instruction(op == kOpIEqual ? kOpAll : kOpAny, 4), w[1], w[2], each});
          i = next;
          continue;
        }
        break;
      case kOpUConvert:
        // The low half.
        if (count == 4 && is_wide(w[3])) {
          emit({instruction(kOpCompositeExtract, 5), w[1], w[2], w[3], 0u});
          i = next;
          continue;
        }
        break;
      case kOpConvertUToPtr:
        if (count == 4 && is_wide(w[3])) {
          emit({instruction(kOpBitcast, 4), w[1], w[2], w[3]});
          i = next;
          continue;
        }
        break;
      default:
        break;
    }
    // Anything else an address goes into as a number is not followed: the comparisons and arithmetic the
    // shaders do not use on one.
    if ((op >= 126 && op <= 148) || (op >= 172 && op <= 183) || op == 194 || op == 195 || op == 114 || op == 112 ||
        op == 111) {
      for (uint32_t k = 3; k < count; ++k) {
        if (is_wide(w[k])) return false;
      }
    }
    copy_retyped(w, count, op);
    i = next;
  }
  out[3] = bound;
  return true;
}

}  // namespace renut::spirv
