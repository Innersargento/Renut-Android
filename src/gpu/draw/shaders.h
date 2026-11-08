#pragma once
// GPU backend subsystem Shaders. See backend.h.
#include "gpu/core/types.h"

namespace renut::gpu {

class GpuBackend;

struct Shaders {
  explicit Shaders(GpuBackend& owner) : backend(owner) {}
  GpuBackend& backend;

  // A draw left out for a reason no other counter names: counted by reason
  // and shader for the periodic report, and named in a trace.
  bool Drop(const char* reason, const HostShader* vs, const HostShader* ps);
  Shader* LoadShaderByContents(xenos::ShaderType shader_type, const uint32_t* host_address,
                                    uint32_t dword_count);
  // A vertex shader load with fetches (kOpShader): the microcode at host_address with the
  // fetches' slots replaced, which is what the guest's own binding used to leave in memory. The
  // shader made is the one for those bytes, so everything downstream sees what it always did.
  // `fetches`: count x {slot, word 0, 1, 2} as the front end read them.
  // `hash` is the hash of the container the shader object was made from (`program` of it), or 0
  // when the front end does not know the object: not of these bytes, which are the program as
  // created and never written.
  Shader* LoadShaderPatched(xenos::ShaderType shader_type, uint64_t hash, uint32_t program,
                            const uint32_t* host_address, uint32_t dword_count, const uint32_t* fetches,
                            uint32_t count);
  // A load named by identity, no fetches: the shader made of the bytes the first time the
  // container's program is seen, found by it after that.
  Shader* LoadShaderIdentity(xenos::ShaderType shader_type, uint64_t container, uint32_t program,
                             const uint32_t* host_address, uint32_t dword_count);
  std::unordered_map<uint64_t, HostShader*> identity_shaders_;
  // In front of identity_shaders_ (a load for nearly every shader bind of every draw).
  LookupMemo<HostShader, 12> identity_memo_;
  // (the microcode's hash, the fetches) -> the shader they make; the microcode behind a hash is
  // not written, so it holds.
  std::unordered_map<uint64_t, HostShader*> patched_shaders_;
  // `container`: the container of the shader object (0: unknown). One shader per container for the
  // same bytes, and its entry the container's own when the keys agree (ShaderStore::FindOwn).
  HostShader* FindOrMakeShader(xenos::ShaderType shader_type, uint64_t hash, const uint32_t* host_address,
                               uint32_t dword_count, uint64_t container = 0);
  // Entries found by the container / by the keys, for loads that named a container.
  uint32_t own_entries_ = 0, key_entries_ = 0;
  std::unordered_map<uint64_t, std::unique_ptr<HostShader>> shaders_;
  std::unordered_map<const uint32_t*, std::pair<uint32_t, HostShader*>> shader_lookup_;
  // In front of shader_lookup_, by address and length; cleared with it.
  LookupMemo<HostShader, 12> shader_memo_;
  uint64_t shader_lookup_frame_ = UINT64_MAX;
  std::unordered_set<uint64_t> missed_;
};

}  // namespace renut::gpu
