#pragma once
/**
 * A Xenos shader's microcode, kept in host byte order, and what the backend asks of it.
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    reNut - taken into the app (renut::gpu::xshader). The shaders run from the offline
 *              recompilation (generated/shaders), so the translator, the disassembler and the
 *              bindings are gone; what is left is the walk of the control flow that finds the boolean
 *              constants the program tests (the Hi-Z culler specializes its model on them) and
 *              whether it samples a texture (its interpreter does not).
 */

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <rex/graphics/format/ucode.h>
#include <rex/graphics/xenos.h>
#include <rex/memory.h>

namespace renut::gpu {
namespace xshader {
namespace xenos = rex::graphics::xenos;
namespace memory = rex::memory;

class Shader {
 public:
  // `ucode_source_endian` is the byte order of `ucode_dwords`; inside the Shader the ucode is in the
  // host's.
  Shader(xenos::ShaderType shader_type, uint64_t ucode_data_hash, const uint32_t* ucode_dwords,
         size_t ucode_dword_count, std::endian ucode_source_endian = std::endian::big);
  virtual ~Shader() = default;

  xenos::ShaderType type() const { return shader_type_; }

  // Microcode dwords in host endianness.
  const std::vector<uint32_t>& ucode_data() const { return ucode_data_; }
  uint64_t ucode_data_hash() const { return ucode_data_hash_; }
  const uint32_t* ucode_dwords() const { return ucode_data_.data(); }
  size_t ucode_dword_count() const { return ucode_data_.size(); }

  bool is_ucode_analyzed() const { return is_ucode_analyzed_; }
  // Walks the control flow program: the following two are valid after it.
  void AnalyzeUcode();

  // Bitmap of the boolean constants the control flow tests (conditional exec, call and jump), a bit
  // per constant in words of 32.
  const uint32_t* bool_bitmap() const { return bool_bitmap_; }
  // Whether an instruction that executes samples a texture into a register the shader goes on to use.
  bool uses_texture_fetch_instruction_results() const { return uses_texture_fetch_instruction_results_; }

 private:
  void GatherExec(uint32_t address, uint32_t count, uint32_t sequence);

  xenos::ShaderType shader_type_;
  uint64_t ucode_data_hash_;
  std::vector<uint32_t> ucode_data_;
  bool is_ucode_analyzed_ = false;
  uint32_t bool_bitmap_[256 / 32] = {};
  bool uses_texture_fetch_instruction_results_ = false;
};

}  // namespace xshader
}  // namespace renut::gpu
