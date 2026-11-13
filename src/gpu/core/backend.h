#pragma once
// The GPU backend: the lists of d3d/frontend/, executed on Plume. It owns the subsystems.
#include "gpu/draw/pipelines.h"
#include "gpu/draw/streams.h"
#include "gpu/targets/targets.h"
#include "gpu/targets/textures.h"
#include "gpu/draw/shaders.h"
#include "gpu/exec/lists.h"
#include "gpu/draw/draw.h"
#include "gpu/exec/recorder.h"
#include "gpu/draw/hiz.h"
#include "gpu/targets/resolve.h"
#include "gpu/draw/patches.h"
#include "gpu/targets/swap.h"

namespace renut::gpu {

class GpuBackend final {
 public:
  GpuBackend(rex::memory::Memory* memory, rex::system::KernelState* kernel);
  // Starts the worker ("GPU Commands"): SetupContext runs on it, then the loop.
  bool Initialize();
  void Shutdown();
  // Wakes the worker: a list was published.
  void Wake();
  uint32_t counter() const;
  void increment_counter();
  void add_counter(uint32_t n);
  DrawState& state() { return state_; }
  Shader* active_vertex_shader() const;
  Shader* active_pixel_shader() const;
  rex::memory::Memory* memory_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  // Calls a guest callback (function, context) as the CPU it names: a callback record of a list.
  std::function<void(uint32_t, uint32_t, uint32_t)> dispatch_callback_;
  // Brings the vertical blank's clock up to date (host.cpp TickVblank): called before a present or a
  // callback record is run, on the backend's thread. Empty when a thread ticks it.
  std::function<void()> tick_vblank_;
  // The state the lists write and the draws read (gpu/core/draw_state.h).
  DrawState state_;
  std::atomic<bool> worker_running_;
  rex::system::object_ref<rex::system::XHostThread> worker_thread_;
  uint32_t counter_ = 0;
  std::unique_ptr<rex::thread::Event> wake_;
  bool SetupContext();
  void ShutdownContext();
  // The GPU backend's work loop. The SDK runs its own on the worker
  // thread it creates, which calls SetupContext() first: this one runs from
  // there until the processor is shut down, so the SDK's loop is never entered.
  // It does what the SDK's does and, besides, the native work.
  void RunWorkerLoop();
  // ---- Native work ----
  // Lists the front end published are run by the worker itself.
  bool NativeWorkPending();
  void RunNativeWork();
  bool wake_registered_ = false;
  bool native_started_ = false;
  VkShaderModule pack_module_ = VK_NULL_HANDLE;
  VkPipelineLayout pack_layout_ = VK_NULL_HANDLE;
  VkShaderModule patch_modules_[2] = {};
  // Host memory profile (see InitializeContext). A phone with 3 GB of RAM
  // shares it with the GPU: the ring, the textures and the kept streams are
  // the whole of what the driver holds, and left at the desktop-class sizes
  // they pushed the game past what Android lets a process keep - the system
  // killed it (or the Mali lost its device) as the first scene loaded.
  bool low_memory_ = false;
  uint64_t upload_slice_size_ = kUploadSize;
  uint32_t max_texture_dim_ = 0;
  // BC textures: the device samples them, and files/plume-bc / the cvar want them.
  bool bc_supported_ = false;
  // Bit n (BcKind n): the device makes a 3D image of that block format (the stacked terrain textures).
  uint32_t bc3d_mask_ = 0;
  // 0 full, 1 half, 2 quarter: mips of a mip-filtered 2D texture not kept on the host.
  uint32_t texture_quality_ = 0;
  void* watch_handle_ = nullptr;
  RenderDevice* device_ = nullptr;
  RenderCommandQueue* queue_ = nullptr;
  std::string miss_dump_directory_;
  std::unique_ptr<RenderPipelineLayout> layout_;
  std::unique_ptr<RenderDescriptorSet> set_2d_, set_3d_, set_cube_, set_samplers_;
  std::array<std::unique_ptr<RenderSampler>, kMaxSamplers> samplers_;
  std::unique_ptr<RenderTexture> white_2d_, white_3d_, white_cube_;
  std::unique_ptr<RenderTextureView> white_cube_view_;
  uint8_t* upload_data_ = nullptr;
  uint64_t upload_address_ = 0;
  uint64_t zero_offset_ = 0;
  // The same three blocks as uniform buffers (set 4 in shader_common.h): one
  // descriptor set over the whole ring, bound with each draw's offsets. The
  // recompiled shaders read every constant through these; the push constants
  // stay for the geometry shaders, which read the shared block's address.
  VkDescriptorSetLayout constants_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool constants_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet constants_set_ = VK_NULL_HANDLE;
  bool CreateConstantBuffers();
  // Per-draw textures (PlumePerDrawTextures: a device whose limits leave out the heaps, the Mali-G52). The
  // four heaps above are never bound, only copied from: each draw binds set 0 of its own 32 2D, 3D and cube
  // textures and 32 samplers in fetch-slot order, its shared block holds the slot numbers where the heap
  // indices were, and the constants are set 1 (spirv_descriptor_sets.h rewrites the modules to match).
  bool per_draw_textures_ = false;
  uint32_t ConstantSet() const {
    return per_draw_textures_ ? spirv::kPerDrawConstantSet : spirv::kCompiledConstantSet;
  }
  // Bumped by every write into a heap: a set copied before it may hold what an index used to be.
  uint64_t heap_epoch_ = 0;
  VkDescriptorSetLayout draw_set_layout_ = VK_NULL_HANDLE;
  struct DrawTextureKey {
    uint16_t index[4 * spirv::kPerDrawTextureSlots];
    bool operator==(const DrawTextureKey& other) const { return std::memcmp(index, other.index, sizeof(index)) == 0; }
  };
  struct DrawTextureKeyHash {
    size_t operator()(const DrawTextureKey& key) const { return size_t(XXH3_64bits(key.index, sizeof(key.index))); }
  };
  // One frame in flight's sets, freed together once its fence has passed (ResetDrawTextureSets). Draws that
  // name the same textures share one, until a heap write.
  struct DrawTextureSets {
    std::vector<VkDescriptorPool> pools;
    size_t current = 0;
    std::unordered_map<DrawTextureKey, VkDescriptorSet, DrawTextureKeyHash> cache;
    uint64_t epoch = 0;
    uint64_t built = 0;
  };
  std::array<DrawTextureSets, kPlumeFramesInFlight> draw_texture_sets_;
  // The set of a draw whose fetch slots hold these heap indices (32 of each); null if none could be made.
  VkDescriptorSet DrawTextureSet(const uint32_t* textures_2d, const uint32_t* textures_3d, const uint32_t* cubes,
                                 const uint32_t* samplers);
  void ResetDrawTextureSets(uint32_t slot);
  void DestroyDrawTextureSets();
  bool trail_device_default_ = false;
  std::array<std::unique_ptr<RenderCommandList>, kPlumeFramesInFlight> lists_;
  uint32_t slot_ = kPlumeFramesInFlight - 1;
  // frame a pair was first recorded
  std::atomic<bool> device_watch_stop_{false};
  WatchThread device_watch_{device_watch_stop_};
  bool no_shadows_ = false;
  bool dedupe_descriptors_ = false;
  std::array<ResolvedLookup, kResolvedLookupSize> resolved_lookup_{};
  uint64_t resolved_lookup_serial_ = 1;
  uint64_t resolved_lookup_frame_ = UINT64_MAX;
  Stats stats_;
  uint64_t swaps_ = 0;

  // The subsystems.
  Pipelines pipelines{*this};
  Streams streams{*this};
  Targets targets{*this};
  Textures textures{*this};
  Shaders shaders{*this};
  Lists lists{*this};
  Draw draw{*this};
  Recorder recorder{*this};
  HiZ hiz{*this};
  Resolve resolve{*this};
  Patches patches{*this};
  Swap swap{*this};
};


inline GpuBackend::GpuBackend(rex::memory::Memory* memory, rex::system::KernelState* kernel)
      : memory_(memory),
        kernel_state_(kernel),
        worker_running_(true),
        wake_(rex::thread::Event::CreateAutoResetEvent(false)) {}

inline bool GpuBackend::Initialize() {
    worker_running_ = true;
    worker_thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
          if (!SetupContext()) {
            rex::FatalError("Unable to setup GPU backend internal state");
            return 0;
          }
          RunWorkerLoop();
          ShutdownContext();
          return 0;
        }));
    worker_thread_->set_name("GPU Commands");
    worker_thread_->Create();
    return true;
  }

inline void GpuBackend::Shutdown() {
    worker_running_ = false;
    wake_->Set();
    if (worker_thread_) {
      worker_thread_->Wait(0, 0, 0, nullptr);
      worker_thread_.reset();
    }
  }

inline void GpuBackend::Wake() { wake_->Set(); }

inline uint32_t GpuBackend::counter() const { return counter_; }

inline void GpuBackend::increment_counter() { counter_++; }
inline void GpuBackend::add_counter(uint32_t n) { counter_ += n; }

inline Shader* GpuBackend::active_vertex_shader() const { return lists.active_vertex_shader_; }

inline Shader* GpuBackend::active_pixel_shader() const { return lists.active_pixel_shader_; }

inline void Draw::MarkConstantsWritten(uint32_t first, uint32_t count) {
    // `first` is a DrawState word. The two float files are contiguous (vertex c0, then
    // pixel c0) and so are the flags, one per 256 words: the block is the offset
    // shifted. This runs for every constant word the guest writes.
    static_assert(DrawState::kPixelConstants == kConstantChunks * (kConstantChunkSize / 4));
    constexpr uint32_t kShift = 8;
    static_assert((1u << kShift) == kConstantChunkSize / 4);
    constexpr uint32_t kWords = 2 * kConstantChunks << kShift;
    static_assert(kWords == sizeof(DrawState::float_constants) / sizeof(uint32_t));
    if (first < kStateLoopsFirst + 32 && uint64_t(first) + count > kStateLoopsFirst) loop_constants_dirty_ = true;
    if (first < kStateConstantsFirst || !count) return;
    const uint32_t lo = first - kStateConstantsFirst;
    if (lo >= kWords) return;
    const uint32_t hi = std::min(lo + count, kWords) - 1;
    bool* written = &constants_written_[0][0];
    for (uint32_t block = lo >> kShift; block <= hi >> kShift; ++block) written[block] = true;
  }

inline void Lists::StoreConstants(uint32_t first, const uint32_t* values, uint32_t count) {
    uint32_t* dst = reinterpret_cast<uint32_t*>(&backend.state_) + first;
    uint32_t lo = 0;
    while (lo < count && dst[lo] == values[lo]) ++lo;
    if (lo == count) return;
    uint32_t hi = count;
    while (dst[hi - 1] == values[hi - 1]) --hi;
    std::memcpy(dst + lo, values + lo, size_t(hi - lo) * sizeof(uint32_t));
    backend.draw.MarkConstantsWritten(first + lo, hi - lo);
  }

inline void Lists::StoreBig(uint32_t physical, uint32_t value) {
    const uint32_t big = __builtin_bswap32(value);
    std::memcpy(backend.memory_->TranslatePhysical<uint8_t*>(physical), &big, 4);
  }

inline bool GpuBackend::NativeWorkPending() { return native_d3d::WorkPending(); }

inline void GpuBackend::RunNativeWork() {
    native_d3d::TakeWorkPending();
    ExecutorBusy busy;
    lists.RunNative(UINT32_MAX);
  }

inline uint8_t* Draw::Pointer(uint64_t offset) { return backend.upload_data_ + offset; }

inline uint64_t Draw::Address(uint64_t offset) const { return backend.upload_address_ + offset; }

inline void Streams::IndexBlocks(std::unordered_multimap<uint32_t, uint64_t>& index, uint32_t base, uint32_t size,
                          uint64_t key) {
    if (!size) return;
    for (uint32_t block = base >> 16; block <= uint32_t((uint64_t(base) + size - 1) >> 16); ++block) {
      index.emplace(block, key);
    }
  }

inline template <typename... Args>
  void Draw::Trace(fmt::format_string<Args...> format, Args&&... args) {
    if (trace_) {
      fmt::print(trace_, format, std::forward<Args>(args)...);
      std::fputc(10, trace_);
    }
  }

inline void Recorder::BindIndexBuffer(RenderBuffer* buffer, RenderFormat format) {
    if (backend.targets.bound_index_buffer_ == buffer && backend.targets.bound_index_format_ == format) return;
    // Vulkan binds an index buffer by offset alone; the size is not used.
    RenderIndexBufferView view(RenderBufferReference(buffer, 0), 0, format);
    L()->setIndexBuffer(&view);
    backend.targets.bound_index_buffer_ = buffer;
    backend.targets.bound_index_format_ = format;
  }

inline RenderCommandList* Recorder::L() {
    // The recording thread's own calls get the list as it is.
    if (OnRecorder()) return backend.targets.list_;
    if (!deferred_.empty()) FlushDeferred(backend.draw.in_issue_draw_ ? 4 : 0);
    if (rec_posted_.load(std::memory_order_relaxed) != rec_completed_.load(std::memory_order_relaxed) || rec_pending_) {
      SyncRecorder();
    }
    return backend.targets.list_;
  }

inline bool Recorder::OnRecorder() const { return std::this_thread::get_id() == rec_thread_id_; }

inline bool Recorder::RecActive() const {
    return backend.swap.rec_enabled_ && !backend.draw.sync_draws_ && sync_every_ <= 1 && !backend.draw.trace_;
  }

inline std::shared_ptr<PipelineJob> Pipelines::TakeUrgentJob() {
    const uint64_t now = backend.swap.pipeline_frame_.load(std::memory_order_relaxed);
    auto best = pipeline_urgent_queue_.begin();
    uint64_t best_score = 0;
    for (auto it = pipeline_urgent_queue_.begin(); it != pipeline_urgent_queue_.end(); ++it) {
      const PipelineJob& job = **it;
      const uint64_t recent = now - std::min(now, job.wanted_frame.load(std::memory_order_relaxed)) <= 3;
      const uint64_t score = (recent << 32) | job.demand.load(std::memory_order_relaxed);
      if (score > best_score) {
        best_score = score;
        best = it;
      }
    }
    std::shared_ptr<PipelineJob> job = std::move(*best);
    pipeline_urgent_queue_.erase(best);
    return job;
  }

inline uint32_t Targets::AllocateTextureDescriptor() {
    uint32_t index = 0;
    if (!free_descriptors_.empty()) {
      index = free_descriptors_.back();
      free_descriptors_.pop_back();
    } else if (next_descriptor_ < kMaxTextures2D) {
      index = next_descriptor_++;
    }
    written_2d_[index] = {};
    return index;
  }

inline void Targets::SetTexture2D(uint32_t index, const RenderTexture* texture, const RenderTextureView* view) {
    WrittenDescriptor& written = written_2d_[index];
    if (backend.dedupe_descriptors_ && written.texture == texture && written.view == view) {
      ++backend.recorder.descriptor_writes_skipped_;
      return;
    }
    written = {texture, view};
    ++backend.recorder.descriptor_writes_;
    backend.set_2d_->setTexture(index, texture, RenderTextureLayout::SHADER_READ, view);
    ++backend.heap_epoch_;
  }

inline uint32_t Textures::Allocate3DDescriptor() {
    if (!backend.targets.free_3d_descriptors_.empty()) {
      uint32_t index = backend.targets.free_3d_descriptors_.back();
      backend.targets.free_3d_descriptors_.pop_back();
      return index;
    }
    return backend.targets.next_3d_descriptor_ < kMaxTextures3D ? backend.targets.next_3d_descriptor_++ : 0;
  }

inline uint32_t Textures::AllocateCubeDescriptor() {
    if (!backend.targets.free_cube_descriptors_.empty()) {
      uint32_t index = backend.targets.free_cube_descriptors_.back();
      backend.targets.free_cube_descriptors_.pop_back();
      return index;
    }
    return backend.targets.next_cube_descriptor_ < kMaxTexturesCube ? backend.targets.next_cube_descriptor_++ : 0;
  }

inline void Targets::RetireTexture(std::unique_ptr<RenderTexture>& texture, uint32_t& descriptor) {
    if (texture) retired_slots_[backend.slot_].push_back(std::move(texture));
    if (descriptor) retired_descriptor_slots_[backend.slot_].push_back(descriptor);
    descriptor = 0;
  }

inline void Targets::RetireGuestTexture(GuestTexture& texture) {
    if (texture.view) retired_views_[backend.slot_].push_back(std::move(texture.view));
    RetireTexture(texture.texture, texture.descriptor);
  }

inline void Targets::RetireSwizzled(Resolved& r) {
    for (auto& [key, s] : r.swizzled) {
      if (s.view) retired_views_[backend.slot_].push_back(std::move(s.view));
      if (s.descriptor) retired_descriptor_slots_[backend.slot_].push_back(s.descriptor);
    }
    r.swizzled.clear();
  }

inline uint64_t Targets::ResolvedKey(uint32_t address, RenderFormat format) {
    return uint64_t(address) | (uint64_t(format) << 32);
  }

inline Resolved* Draw::FindResolved(uint32_t address) {
    if (backend.resolved_lookup_frame_ != backend.swaps_) {
      backend.resolved_lookup_frame_ = backend.swaps_;
      ++backend.resolved_lookup_serial_;
    }
    // Two ways by a mix of the address, valid for this frame and this serial: a map that was emptied every
    // frame was two hash lookups a sampled slot.
    ResolvedLookup* ways = &backend.resolved_lookup_[ResolvedLookupPair(address)];
    if (ways[0].serial == backend.resolved_lookup_serial_ && ways[0].address == address) return ways[0].value;
    if (ways[1].serial == backend.resolved_lookup_serial_ && ways[1].address == address) return ways[1].value;
    Resolved* latest = nullptr;
    for (auto& [key, r] : backend.targets.resolved_) {
      if (uint32_t(key) == address && r.texture && (!latest || r.written > latest->written)) latest = &r;
    }
    if (latest && latest->checked_frame != backend.swaps_) {
      latest->checked_frame = backend.swaps_;
      const bool stale = native_d3d::TextureMemoryEpoch(address) != latest->memory_epoch;
      if (stale) {
        // Something else lives here now; forget every resolve at the address.
        for (auto it = backend.targets.resolved_.begin(); it != backend.targets.resolved_.end();) {
          if (uint32_t(it->first) == address) {
            backend.targets.RetireSwizzled(it->second);
            if (it->second.alias) backend.targets.RetireFramebuffers();  // a framebuffer may still name its image
            backend.targets.RetireTexture(it->second.texture, it->second.descriptor);
            backend.targets.RetireTexture(it->second.fitted, it->second.fitted_descriptor);
            it = backend.targets.resolved_.erase(it);
          } else {
            ++it;
          }
        }
        backend.targets.InvalidateResolvedLookups();
        return nullptr;
      }
    }
    if (latest) latest->sampled = true;
    ways[1] = ways[0];
    ways[0] = {address, backend.resolved_lookup_serial_, latest};
    return latest;
  }

inline void Targets::InvalidateResolvedLookups() { ++backend.resolved_lookup_serial_; }

// A resolve (or a texture made for one) changes the answer for its own address only: the other cached
// addresses stay valid, where a bump of the serial made every sampled slot of the draws that followed walk the
// map again.
inline void Targets::InvalidateResolvedAddress(uint32_t address) {
  ResolvedLookup* ways = &backend.resolved_lookup_[ResolvedLookupPair(address)];
  for (uint32_t i = 0; i < 2; ++i) {
    if (ways[i].address == address) ways[i].serial = 0;
  }
}

}  // namespace renut::gpu
