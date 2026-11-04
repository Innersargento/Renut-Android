/**
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    reNut - taken into the app (renut::gpu::xshader), see shader.h
 */

#include "gpu/xenos/shader.h"

#include <algorithm>
#include <cstring>

namespace renut::gpu {
namespace xshader {
namespace ucode = rex::graphics::ucode;

using namespace ucode;

Shader::Shader(xenos::ShaderType shader_type, uint64_t ucode_data_hash, const uint32_t* ucode_dwords,
               size_t ucode_dword_count, std::endian ucode_source_endian)
    : shader_type_(shader_type), ucode_data_hash_(ucode_data_hash) {
  ucode_data_.resize(ucode_dword_count);
  if (std::endian::native != ucode_source_endian) {
    memory::copy_and_swap(ucode_data_.data(), ucode_dwords, ucode_dword_count);
  } else {
    std::memcpy(ucode_data_.data(), ucode_dwords, sizeof(uint32_t) * ucode_dword_count);
  }
}

void Shader::AnalyzeUcode() {
  if (is_ucode_analyzed_) return;
  is_ucode_analyzed_ = true;

  // The control flow instructions come in pairs of three dwords, all listed at the top of the
  // microcode. Its length is guessed by the first exec's address (as freedreno does).
  uint32_t bound = uint32_t(ucode_data_.size() / 3);
  for (uint32_t i = 0; i < bound; ++i) {
    ControlFlowInstruction cf_ab[2];
    UnpackControlFlowInstructions(ucode_data_.data() + i * 3, cf_ab);
    for (uint32_t j = 0; j < 2; ++j) {
      if (IsControlFlowOpcodeExec(cf_ab[j].opcode())) bound = std::min(bound, uint32_t(cf_ab[j].exec.address()));
    }
  }

  auto test_bool = [&](uint32_t index) { bool_bitmap_[(index / 32) % 8] |= uint32_t(1) << (index % 32); };
  for (uint32_t i = 0; i < bound; ++i) {
    ControlFlowInstruction cf_ab[2];
    UnpackControlFlowInstructions(ucode_data_.data() + i * 3, cf_ab);
    for (uint32_t j = 0; j < 2; ++j) {
      const ControlFlowInstruction& cf = cf_ab[j];
      switch (cf.opcode()) {
        case ControlFlowOpcode::kCondExec:
        case ControlFlowOpcode::kCondExecEnd:
        case ControlFlowOpcode::kCondExecPredClean:
        case ControlFlowOpcode::kCondExecPredCleanEnd:
          test_bool(cf.cond_exec.bool_address());
          GatherExec(cf.cond_exec.address(), cf.cond_exec.count(), cf.cond_exec.sequence());
          break;
        case ControlFlowOpcode::kExec:
        case ControlFlowOpcode::kExecEnd:
          GatherExec(cf.exec.address(), cf.exec.count(), cf.exec.sequence());
          break;
        case ControlFlowOpcode::kCondExecPred:
        case ControlFlowOpcode::kCondExecPredEnd:
          GatherExec(cf.cond_exec_pred.address(), cf.cond_exec_pred.count(), cf.cond_exec_pred.sequence());
          break;
        case ControlFlowOpcode::kCondCall:
          if (!cf.cond_call.is_unconditional() && !cf.cond_call.is_predicated()) test_bool(cf.cond_call.bool_address());
          break;
        case ControlFlowOpcode::kCondJmp:
          if (!cf.cond_jmp.is_unconditional() && !cf.cond_jmp.is_predicated()) test_bool(cf.cond_jmp.bool_address());
          break;
        default:
          break;
      }
    }
  }
}

// The instructions of one exec clause: two bits of `sequence` each, bit 0 a fetch (else an ALU
// instruction). Only texture fetches whose result reaches a register matter.
void Shader::GatherExec(uint32_t address, uint32_t count, uint32_t sequence) {
  for (uint32_t offset = address; offset < address + count; ++offset, sequence >>= 2) {
    if ((uint64_t(offset) + 1) * 3 > ucode_data_.size()) break;
    if (!(sequence & 0b01)) continue;
    const auto& fetch = *reinterpret_cast<const FetchInstruction*>(ucode_data_.data() + offset * 3);
    switch (fetch.opcode()) {
      case FetchOpcode::kTextureFetch:
      case FetchOpcode::kGetTextureBorderColorFrac:
      case FetchOpcode::kGetTextureComputedLod:
      case FetchOpcode::kGetTextureGradients:
      case FetchOpcode::kGetTextureWeights: {
        // A destination component taken from the fetched value (x to w), not a constant or a keep.
        const uint32_t swizzle = fetch.texture_fetch().dest_swizzle();
        for (uint32_t c = 0; c < 4; ++c) {
          if (uint32_t(GetFetchDestinationComponentSwizzle(swizzle, c)) <= uint32_t(FetchDestinationSwizzle::kW)) {
            uses_texture_fetch_instruction_results_ = true;
          }
        }
      } break;
      default:
        break;
    }
  }
}

}  // namespace xshader
}  // namespace renut::gpu
