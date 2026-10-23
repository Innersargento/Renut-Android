#pragma once
// GPU backend subsystem Lists. See backend.h.
#include "types.h"

namespace renut::gpu {

class GpuBackend;

struct Lists {
  explicit Lists(GpuBackend& owner) : backend(owner) {}
  GpuBackend& backend;

  uint64_t bin_select_ = 0xFFFFFFFFull;
  uint64_t bin_mask_ = 0xFFFFFFFFull;
  Shader* active_vertex_shader_ = nullptr;
  Shader* active_pixel_shader_ = nullptr;
  // A write of `count` registers from `values`: the registers that already hold
  // these bytes are left alone, and only the blocks a different one lands in are
  // marked. The guest sets the same view/projection/lighting constants before
  // draw after draw, and a marked block costs a compare of the whole 1 KB (and
  // an upload when it differs) at the next draw that reads it.
  void StoreConstants(uint32_t first, const uint32_t* values, uint32_t count);
  // Debug: a frame trace lists the draws the bin check drops, and
  // <files>/plume-pred-all "1" runs every predicated packet.
  bool OnPredicatedSkip(uint32_t opcode);
  bool native_presenting_ = false;
  uint64_t native_fences_ = 0, native_presents_ = 0, native_callbacks_ = 0;
  // A dword the GPU writes back (big-endian), at a GPU address.
  void StoreBig(uint32_t physical, uint32_t value);
  BufferRange native_indices_{};
  void NativeEffects(const uint32_t* pairs, uint32_t count);
  uint64_t native_effects_applied_ = 0;
  // Runs the published records until the end, or until the opaque-call
  // marker `until` (consumed; returns true), never past another marker.
  bool RunNative(uint32_t until);
  void NativeWrite(uint32_t first, const uint32_t* values, uint32_t count);
  void NativeDraw(uint32_t flags, uint32_t initiator, uint32_t dma_base, uint32_t dma_size);
  void NativeClear(const uint32_t* record);
  std::vector<NativeFrame> native_stack_;
  std::deque<std::shared_ptr<const native_d3d::List>> native_queue_;
  uint64_t native_draws_ = 0, native_bad_ = 0;
  // The engine pass of the draw being issued (native_d3d::kDrawPassShift).
  uint8_t draw_pass_ = 0;
};

}  // namespace renut::gpu
