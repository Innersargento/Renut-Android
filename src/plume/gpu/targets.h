#pragma once
// GPU backend subsystem Targets. See backend.h.
#include "types.h"

namespace renut::gpu {

class GpuBackend;

struct Targets {
  explicit Targets(GpuBackend& owner) : backend(owner) {}
  GpuBackend& backend;

  uint64_t native_waits_ = 0, native_wait_timeouts_ = 0, native_wait_us_ = 0;
  // the hint GuestStream reads
  uint64_t object_streams_ = 0, window_streams_ = 0;
  std::array<ListSurface, 5> native_surfaces_{};
  // The target of a slot's surface; `info_register` is what the register
  // file holds for it, which must be the surface's.
  Target* NativeTarget(uint32_t slot, uint32_t info_register);
  Target* SurfaceTarget(uint32_t slot);
  // A framebuffer is as big as its first attachment: the others grow to it.
  void MatchSurfaceSizes(std::array<Target*, 4>& colors, uint32_t color_count, Target*& depth);
  bool GrowSurfaceTarget(Target& target, uint32_t width, uint32_t height);
  void RetireFramebuffers();
  void EvictSurfaceTargets();
  std::string SurfaceReport();
  uint64_t surface_created_ = 0, surface_grown_ = 0, surface_evicted_ = 0, surface_mismatch_ = 0;
  // Native resolves into their texture object, and the ones that go by
  // address (3D slices).
  uint64_t object_resolves_ = 0, address_resolves_count_ = 0;
  uint64_t surface_missing_ = 0;
  // Quarters of the guest's resolution the dynamic shadow maps (depth D24S8
  // surfaces of 640 px and more) are drawn at: 4, 3 or 2.
  uint32_t shadow_scale_ = 4;
  // The same for the scene's own 1280x720 colour and depth surfaces.
  uint32_t internal_scale_ = 4;
  uint64_t texture_budget_ = 0;
  void BeginFrame();
  void EvictIdleTextures();
  void StartList();
  void Flush();
  void Transition(RenderTexture* texture, RenderTextureLayout& current, RenderTextureLayout layout);
  RenderFramebuffer* GetFramebuffer(const std::array<Target*, 4>& colors, uint32_t color_count,
                                    Target* depth);
  void SetFramebuffer(RenderFramebuffer* framebuffer);
  uint32_t FittedResolved(Resolved& resolved, uint32_t width, uint32_t height);
  Resolved& GetResolved(uint32_t address, RenderFormat format, uint32_t width, uint32_t height);
  // A depth map resolved whole into a texture object, without the copy: the
  // surface's image becomes the resolved texture and the surface takes another
  // (files/plume-depthalias, default on). nullptr when no image was available.
  Resolved* AliasDepthResolve(Target& source, uint32_t address);
  // Copies the contents back into a surface whose image went to a resolve, for a
  // draw or a partial clear that uses them. A clear of the whole surface drops
  // them instead (true: the image is fresh, so its stencil is undefined too).
  bool RestoreAliasedDepth(Target& target, bool full_clear);
  void NoteAliasedUse(Target& target, bool whole_clear);
  // Debug: files/plume-retire-delay (frames) keeps retired buffers alive that much longer than the
  // frames in flight need, to tell a read past a buffer's end from a read of one freed too soon.
  std::deque<std::pair<uint64_t, std::shared_ptr<void>>> delayed_buffers_;
  uint64_t retire_delay_ = 0;
  uint32_t retire_mask_ = 15;
  // The addresses last pushed, to leave the push constants alone when the
  // three blocks are the ones already bound.
  uint64_t pushed_constants_[3] = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
  std::array<VertexBinding, 17> bound_vertex_buffers_{};
  const RenderBuffer* bound_index_buffer_ = nullptr;
  RenderFormat bound_index_format_ = RenderFormat::UNKNOWN;
  std::array<std::vector<PassMark>, kPlumeFramesInFlight> pass_marks_;
  uint32_t color_count_of_pass_ = 0;
  bool depth_of_pass_ = false;
  bool pair_capture_ = false;
  RenderCommandList* list_ = nullptr;
  // Where the current slot's slice of the ring starts.
  uint64_t slice_base_ = 0;
  // Permanent allocations (white texel, zeros) live before the first slice.
  uint64_t slice_origin_ = 0;
  RenderFramebuffer* bound_framebuffer_ = nullptr;
  bool state_bound_ = false;
  // The pass attachments of the draw being recorded, on the recorder's side
  // (color_count_of_pass_ is this thread's, read by IssueDraw).
  uint32_t emit_colors_ = 0;
  bool emit_depth_ = false;
  std::unordered_map<uint64_t, Target> targets_;
  std::unordered_map<uint64_t, std::unique_ptr<RenderFramebuffer>> framebuffers_;
  // Keyed by guest address and host format: N&B reuses an address within a
  // frame for different things (a 768x768 shadow map, then the bloom chain),
  // and draws recorded earlier must keep the texture they sampled.
  std::unordered_map<uint64_t, Resolved> resolved_;
  // Textures replaced while the frame's commands may still use them; freed
  // once the frame has run (IssueSwap waits for it).
  std::array<std::vector<std::unique_ptr<RenderTexture>>, kPlumeFramesInFlight> retired_slots_;
  std::array<std::vector<std::unique_ptr<RenderTextureView>>, kPlumeFramesInFlight> retired_views_;
  // A recorded draw stores a bindless index, not a snapshot of that binding.
  // Keep old image bindings alive until the submission has finished as well.
  std::array<std::vector<uint32_t>, kPlumeFramesInFlight> retired_descriptor_slots_;
  std::vector<uint32_t> free_descriptors_;
  uint32_t AllocateTextureDescriptor();
  std::array<WrittenDescriptor, kMaxTextures2D> written_2d_ = {};
  void SetTexture2D(uint32_t index, const RenderTexture* texture, const RenderTextureView* view = nullptr);
  // Volumes and stacks share the 3D set, cube maps have their own; both are far
  // smaller than the 2D one and recycled the same way.
  std::vector<uint32_t> free_3d_descriptors_, free_cube_descriptors_;
  std::array<std::vector<uint32_t>, kPlumeFramesInFlight> retired_3d_descriptor_slots_;
  std::array<std::vector<uint32_t>, kPlumeFramesInFlight> retired_cube_descriptor_slots_;
  uint64_t last_eviction_ = 0;
  void RetireTexture(std::unique_ptr<RenderTexture>& texture, uint32_t& descriptor);
  // A 2D guest texture's image, its view (a swizzled BC texture has one) and descriptor.
  void RetireGuestTexture(GuestTexture& texture);
  void RetireSwizzled(Resolved& r);
  uint32_t SwizzledResolved(Resolved& r, bool fitted, uint32_t swizzle, bool stencil_first);
  std::array<std::vector<std::unique_ptr<RenderFramebuffer>>, kPlumeFramesInFlight>
      retired_framebuffer_slots_;
  static uint64_t ResolvedKey(uint32_t address, RenderFormat format);
  void InvalidateResolvedLookups();
  uint64_t target_serial_ = 0;
  std::unordered_map<uint64_t, GuestTexture> guest_textures_;
  // the slot IssueDraw is resolving, for LayoutOf and GetGuestTexture
  uint64_t object_epoch_ = 1;
  LastFramebuffer last_framebuffer_;
  uint32_t next_descriptor_ = 1;
  std::unordered_map<uint64_t, GuestTexture> guest_cubes_;
  uint32_t next_cube_descriptor_ = 1;
  uint32_t next_3d_descriptor_ = 1;
};

}  // namespace renut::gpu
