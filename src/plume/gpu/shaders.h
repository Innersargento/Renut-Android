#pragma once
// GPU backend subsystem Shaders. See backend.h.
#include "types.h"

namespace renut::gpu {

class GpuBackend;

struct Shaders {
  explicit Shaders(GpuBackend& owner) : backend(owner) {}
  GpuBackend& backend;

  std::unordered_map<uint32_t, uint32_t> extended_register_values_;
  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                          const uint32_t* host_address, uint32_t dword_count);
  // Register writes. The shader constants (ALU, fetch, boolean and loop,
  // 0x4000-0x4927) have no side effects in the base class, and the guest
  // writes thousands of them a frame through one virtual call each: those
  // ranges are swapped in bulk here, and every write into a stage's float
  // constants marks that stage's block as needing a look (CommitConstants).
  void WriteRegister(uint32_t index, uint32_t value);
  // A draw left out for a reason no other counter names: counted by reason
  // and shader for the periodic report, and named in a trace.
  bool Drop(const char* reason, const HostShader* vs, const HostShader* ps);
  Shader* LoadShaderByContents(xenos::ShaderType shader_type, const uint32_t* host_address,
                                    uint32_t dword_count);
  std::unordered_map<uint64_t, std::unique_ptr<HostShader>> shaders_;
  LookupMemo<ShaderWatch, 12> shader_watch_memo_;
  std::unordered_map<const uint32_t*, std::pair<uint32_t, HostShader*>> shader_lookup_;
  // In front of shader_lookup_, by address and length; cleared with it.
  LookupMemo<HostShader, 12> shader_memo_;
  uint64_t shader_lookup_frame_ = UINT64_MAX;
  std::unordered_set<uint64_t> missed_;
};

}  // namespace renut::gpu
