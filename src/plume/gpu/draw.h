#pragma once
// GPU backend subsystem Draw. See backend.h.
#include "types.h"

namespace renut::gpu {

class GpuBackend;

struct Draw {
  explicit Draw(GpuBackend& owner) : backend(owner) {}
  GpuBackend& backend;

  // Marks the constant blocks (kConstantChunks per stage, each bound on its
  // own - see set 4 in shader_common.h) that a write lands in.
  void MarkConstantsWritten(uint32_t first, uint32_t count);
  bool constants_written_[2][kConstantChunks] = {{true, true, true, true}, {true, true, true, true}};
  // Whether the draw being run came from a predicated packet: the draws come
  // from the native lists, which carry no such packet, so this stays false.
  bool packet_predicated_ = false;
  std::array<BufferRange, 18> native_streams_{};
  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit);
  uint8_t* Pointer(uint64_t offset);
  uint64_t Address(uint64_t offset) const;
  bool validate_windows_ = false;
  // Bumped whenever a kept stream's buffer can have gone: what a draw found for
  // a vertex stream last time is good while this, the ring generation and the
  // stream's fetch constant are the same (see the stream loop of IssueDraw).
  uint64_t stream_epoch_ = 1;
  // Bumped whenever the set of export ranges changes (what overlaps what).
  uint64_t export_epoch_ = 1;
  std::array<StreamMemo, 17> stream_memo_{};
  std::vector<RenderInputElement> draw_input_elements_;
  std::vector<RenderInputSlot> draw_input_slots_;
  std::vector<RenderSpecConstant> draw_spec_constants_;
  std::unordered_set<uint64_t> missed_pairs_;
  // One-frame trace: touching <files>/plume-trace-request records the next
  // frame's draws and resolves into <files>/plume-trace.txt.
  FILE* trace_ = nullptr;
  uint64_t trace_ps_ = 0;
  // A shader hash (hex) in the arm file: no learning, only draws with that
  // vertex or pixel shader fire a burst.
  uint64_t arm_target_ = 0;
  // Debug: <files>/plume-skip-shader holds a shader hash (hex); draws with
  // that vertex or pixel shader are dropped.
  uint64_t skip_shader_ = 0;
  // Debug: <files>/plume-debug-shader holds a shader hash (hex); draws with
  // that vertex or pixel shader lose depth testing, blending, alpha test and
  // culling, to show where they land and what colour they come out.
  uint64_t debug_shader_ = 0;
  // "<hash> <mode>": 1 drops the depth test, 2 blending, 4 alpha test and
  // culling (7 when left out); 8 draws a point list's sprite coordinate; 16
  // and 32 whiten textures (see the shared block).
  uint32_t debug_mode_ = 7;
  std::array<TwinEntry, 128> twin_ring_{};
  uint32_t twin_pos_ = 0;
  template <typename... Args>
  void Trace(fmt::format_string<Args...> format, Args&&... args);
  std::unique_ptr<RenderShader> rect_list_gs_;
  std::unique_ptr<RenderShader> point_list_gs_;
  // which classes: 1 framebuffers, 2 textures, 4 views, 8 buffers

  std::unique_ptr<RenderBuffer> upload_;
  // Counted up every time the ring restarts, so a draw can tell that the
  // blocks it placed there were dropped underneath it.
  uint64_t upload_generation_ = 0;
  alignas(16) uint8_t vs_shadow_[kStageConstantSize] = {};
  alignas(16) uint8_t ps_shadow_[kStageConstantSize] = {};
  alignas(16) uint8_t shared_shadow_[RENUT_SHARED_SIZE] = {};
  // Where a draw assembles its shared block before it is compared.
  alignas(16) uint8_t shared_built_[RENUT_SHARED_SIZE] = {};
  // Where each stage's constant blocks were last placed in the ring.
  uint64_t constant_offsets_[2][kConstantChunks] = {
      {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX}, {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX}};
  uint64_t shared_offset_ = UINT64_MAX;
  // And its second half, from RENUT_SHARED_HOT_SIZE on (binding 9).
  uint64_t shared_cold_offset_ = UINT64_MAX;
  // The loop constants go into that half only when the guest wrote one.
  bool loop_constants_dirty_ = true;
  // files/plume-relconst (or the cvar): how a vertex shader's relative constant
  // reads (a0, aL) are served - 0 by the four blocks, 1 by the whole file as one
  // block, 2 by the same block declared too large for the driver to promote
  // into the constant file. Part of the pipeline key through spec constant 12.
  // Default 2: measured on identical frames (tools/quick-ab.py --interleave) mode
  // 1 costs +8% on the 108 vertex shaders that read by a0/aL, mode 2 -27%, and a
  // frame costs 22% fewer GPU cycles in the heavy plaza (tools/quick-fps.py).
  uint32_t rel_constants_mode_ = 0;
  // Modes 11 and 12 alternate between mode 0 and mode 1 or 2 from one draw of a
  // shader to the next, and mark the alternate's vertex shader hash in the pair
  // profile (bit 60 and 61 xored in), so a single capture holds both variants
  // of the same draws: tools/quick-ab.py --interleave.
  uint32_t rel_toggle_[256] = {};
  // Where the whole vertex file was last placed in the ring, and what it held.
  uint64_t vs_all_offset_ = UINT64_MAX;
  alignas(16) uint8_t vs_all_shadow_[4096] = {};
  // Debug: <files>/plume-syncdraws submits and waits (3 s at most) after every
  // draw, so a draw that takes the GPU down is the last one named in the log.
  // Draws stop being deferred while it is on.
  bool sync_draws_ = false;
  std::unordered_map<uint64_t, uint32_t> sync_diag_;
  std::vector<DeferredSlot> deferred_slots_, flushing_slots_, draw_slots_;
  uint64_t deferred_signature_ = 0;
  bool in_issue_draw_ = false;
  uint32_t hiz_timer_phase_ = 0;
  LookupMemo<std::unique_ptr<RenderPipeline>, 13> pipeline_memo_;
  // Lookups at the draw (renut_plume_pipeline_lookup_ms): whether they are
  // made, the budget per frame and what this frame has spent of it, and the
  // lookups that took a compile's time (a driver ignoring the request).
  bool lookup_here_ = false;
  uint64_t lookup_budget_us_ = 0, lookup_spent_us_ = 0;
  uint32_t slow_lookups_ = 0;
  // The newest ready pipeline of each family, drawn in place of a sibling
  // that is still compiling. Points into pipelines_, which never erases.
  std::unordered_map<uint64_t, RenderPipeline*> standins_;
  std::unordered_map<uint64_t, std::shared_ptr<PipelineJob>> pending_pipelines_;
  std::unordered_set<uint64_t> failed_pipelines_;
  // The texture a fetch at `address` sees: whichever resolve wrote there
  // last, as the memory would hold. The same address is a depth resolve in one
  // phase and a colour resolve in another (the depth-of-field pass also reads
  // the scene depth as 8_8_8_8), and a stale entry of the other kind must not
  // shadow the current one.
  //
  // Asked for every sampled slot of every draw, and nearly always to hear that
  // the texture is not a resolve at all, it walked the whole map each time.
  // The answer only changes when a resolve is made, written or dropped, so it
  // is kept per address until then (InvalidateResolvedLookups), and per frame,
  // where the content check below runs once.
  Resolved* FindResolved(uint32_t address);
  std::vector<PooledStream> stream_pool_ = std::vector<PooledStream>(kStreamPoolSize);
  // Vertex shaders whose patch draws were dropped for want of a domain form, logged once each.
  std::unordered_set<uint64_t> patch_drop_logged_;
};

}  // namespace renut::gpu
