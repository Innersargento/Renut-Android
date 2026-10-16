#pragma once
// GPU backend subsystem Textures. See backend.h.
#include "types.h"

namespace renut::gpu {

class GpuBackend;

struct Textures {
  explicit Textures(GpuBackend& owner) : backend(owner) {}
  GpuBackend& backend;

  const FetchLayout* LayoutOf(const xenos::xe_gpu_texture_fetch_t& fetch);
  const FetchLayout* LayoutOfByContents(const xenos::xe_gpu_texture_fetch_t& fetch);
  uint32_t GetGuestTexture(const xenos::xe_gpu_texture_fetch_t& fetch);
  uint32_t GetGuestCubeTexture(const xenos::xe_gpu_texture_fetch_t& fetch);
  uint32_t GetGuestStackedTexture(const xenos::xe_gpu_texture_fetch_t& fetch);
  uint32_t GetGuestVolumeTexture(const xenos::xe_gpu_texture_fetch_t& fetch);
  uint32_t GetSampler(const xenos::xe_gpu_texture_fetch_t& fetch);
  // Never invalidated: a fetch constant always describes the same layout. The
  // contents behind it are checked separately, once a frame, in GetGuestTexture.
  std::unordered_map<uint64_t, FetchLayout> fetch_layouts_;
  LookupMemo<FetchLayout, 12> layout_memo_;
  // Debug: touching <files>/plume-texdump-request re-decodes every texture
  // the next frame uses and writes each to <files>/texdump/ (raw RGBA).
  std::string texdump_directory_;
  std::unordered_set<uint64_t> dumped_;
  void DumpTexture(const xenos::xe_gpu_texture_fetch_t& fetch, const std::vector<uint32_t>& rgba,
                   uint32_t width, uint32_t height, const char* kind);
  std::unordered_map<uint32_t, uint32_t> sampler_indices_;
  // Nothing is ever erased from sampler_indices_, so its nodes stay put.
  LookupMemo<uint32_t, 8> sampler_memo_;
  uint32_t next_sampler_ = 0;
  uint32_t Allocate3DDescriptor();
  uint32_t AllocateCubeDescriptor();
  LookupMemo<GuestTexture, 12> texture_memo_;
  std::array<SlotObject, 32> slot_objects_{};
  int fetch_slot_ = -1;
  bool objkeys_ = true;
  std::unordered_set<uint32_t> unsupported_formats_;
  std::unordered_map<uint64_t, GuestTexture> guest_stacks_;
  std::unordered_map<uint64_t, GuestTexture> guest_volumes_;
};

}  // namespace renut::gpu
