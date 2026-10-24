#pragma once
// GPU backend subsystem Recorder. See backend.h.
#include "types.h"

namespace renut::gpu {

class GpuBackend;

struct Recorder {
  explicit Recorder(GpuBackend& owner) : backend(owner) {}
  GpuBackend& backend;

  // Predicated tiling, done once. The 2x MSAA scene does not fit the 10 MB of
  // EDRAM, so Direct3D replays it for a lower band with the window offset
  // moved up by the band's height, and the game draws a few objects per band
  // itself (window offset 0, a viewport the size of the band). Host targets
  // have no size limit: the upper band's full-screen draws get the whole
  // screen, the lower band's replay is dropped, the game's own lower-band
  // draws move down by the band's height, and the resolves follow. Learned
  // from a frame that replayed a band, applied from the next one on.
  // Off by default: the water then reflects up close instead of in the
  // distance (it reads something per band that the merge does not give back
  // yet). <files>/plume-merge-bands "1" turns it on: ~30 FPS in Showdown Town.
  // Debug: <files>/plume-bind, per-draw state the draw path leaves as it was
  // bound, to price each on the GPU (the picture is wrong). 1 constants
  // descriptor set, 2 push constants, 4 vertex buffers, 8 viewport and scissor,
  // 16 drops depth-only draws.
  uint32_t bind_skip_ = 0;
  uint32_t bound_constant_offsets_[kConstantBindings];
  // What the list has bound, to leave unchanged state alone (StartList resets).
  RenderViewport bound_viewport_;
  RenderRect bound_scissor_;
  bool viewport_bound_ = false, scissor_bound_ = false;
  void BindIndexBuffer(RenderBuffer* buffer, RenderFormat format);
  std::unique_ptr<RenderCommandFence> fence_;
  bool list_open_ = false;
  const RenderPipeline* bound_pipeline_ = nullptr;
  uint64_t bound_interface_key_ = 0;
  std::vector<RecentDraw> recent_draws_ = std::vector<RecentDraw>(kRecentDraws);
  std::atomic<uint64_t> recent_draw_count_{0};
  std::unordered_map<uint64_t, uint64_t> pair_first_frame_;
  void ReportDeviceLost();
  // files/plume-syncwatch (hex hash): a synced draw whose vertex or pixel shader is this one is logged whole.
  uint64_t sync_watch_ = 0;
  uint32_t sync_watch_count_ = 0;
  uint64_t sync_draw_count_ = 0;
  uint64_t sync_stale_streams_ = 0;
  // streams, over all synced draws, whose kept window differed from guest memory
  uint32_t sync_stale_logged_ = 0;
  std::unordered_set<uint64_t> sync_seen_;
  // The first draw of each pair that ran to completion, for the line a failure prints.
  std::unordered_map<uint64_t, std::string> sync_first_ok_;
  std::string sync_draw_info_;
  std::array<std::string, 6> sync_recent_;
  void SyncAfterDraw(const DrawPacket& pk);
  // files/plume-syncevery (N > 1): the list is submitted and waited for after every Nth draw, with
  // none of plume-syncdraws' bookkeeping - how many draws in flight the failure needs.
  void SyncEvery();
  uint32_t sync_every_ = 0, sync_every_counter_ = 0;
  std::vector<DrawPacket> deferred_, flushing_;
  std::vector<uint32_t> defer_order_, defer_group_start_;
  std::unordered_map<const RenderPipeline*, uint32_t> defer_groups_;
  // cvar renut_plume_no_shadows
  void FlushDeferred(uint32_t reason = 0);
  void EmitDraw(const DrawPacket& pk, const DeferredSlot* slots, uint32_t slot_count);
  // A draw ready to record: queued for the recording thread, or recorded here.
  void Submit(const DrawPacket& pk, const DeferredSlot* slots, uint32_t slot_count);
  // The command list, after recording any draws that are waiting.
  RenderCommandList* L();
  std::mutex rec_mutex_;
  std::condition_variable rec_cv_, rec_done_cv_;
  std::deque<RecJob*> rec_queue_;
  std::atomic<uint32_t> rec_queued_{0};
  std::vector<RecJob*> rec_free_;
  std::vector<std::unique_ptr<RecJob>> rec_owned_;
  RecJob* rec_pending_ = nullptr;
  // being filled by this thread
  std::atomic<uint64_t> rec_posted_{0}
  , rec_completed_{0};
  bool rec_stop_ = false;
  std::thread::id rec_thread_id_;
  RecWorker rec_worker_;
  bool OnRecorder() const;
  bool RecActive() const;
  void RecQueue(const DrawPacket& pk, const DeferredSlot* slots, uint32_t slot_count);
  void RecPost();
  void SyncRecorder();
  void RecThreadMain();
  uint64_t descriptor_writes_ = 0, descriptor_writes_skipped_ = 0;
};

}  // namespace renut::gpu
