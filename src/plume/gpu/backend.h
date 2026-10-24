#pragma once
// The GPU backend: the lists of d3d/native/, executed on Plume. It owns the subsystems.
#include "pipelines.h"
#include "streams.h"
#include "profiling.h"
#include "targets.h"
#include "textures.h"
#include "shaders.h"
#include "lists.h"
#include "draw.h"
#include "recorder.h"
#include "hiz.h"
#include "census.h"
#include "resolve.h"
#include "patches.h"
#include "swap.h"
#include "report.h"

namespace renut::gpu {

class GpuBackend final {
 public:
  // `service_hardware` is called by the worker on every pass: the video hardware's own
  // upkeep (the ring's pointers, in host.cpp), true when it had something to do.
  GpuBackend(rex::memory::Memory* memory, rex::system::KernelState* kernel,
                        std::function<void(uint32_t, uint32_t)> dispatch_interrupt,
                        std::function<bool()> service_hardware);
  // Starts the worker ("GPU Commands"): SetupContext runs on it, then the loop.
  bool Initialize();
  void Shutdown();
  // Wakes the worker: a list was published, or the video hardware has work for it.
  void Wake();
  uint32_t counter() const;
  void increment_counter();
  RegisterFile* registers();
  Shader* active_vertex_shader() const;
  Shader* active_pixel_shader() const;
  uint32_t ReadRegisterValue(uint32_t index) const;
  rex::memory::Memory* memory_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  std::function<void(uint32_t, uint32_t)> dispatch_interrupt_;
  // The GPU's registers: the lists write them, the draws read them, and the CPU sees them
  // through the register window (host.cpp).
  RegisterFile registers_;
  RegisterFile* register_file_ = nullptr;
  std::atomic<bool> worker_running_;
  rex::system::object_ref<rex::system::XHostThread> worker_thread_;
  std::function<bool()> service_hardware_;
  uint32_t counter_ = 0;
  std::unique_ptr<rex::thread::Event> wake_;
  bool SetupContext();
  void ShutdownContext();
  // The GPU backend's work loop. The SDK runs its own on the worker
  // thread it creates, which calls SetupContext() first: this one runs from
  // there until the processor is shut down, so the SDK's loop is never entered.
  // It does what the SDK's does and, around the ring packets, the native work.
  void RunWorkerLoop();
  bool native_checked_ = false;
  // ---- PM4-less: work that does not come through the ring ----
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
  // 0 full, 1 half, 2 quarter: mips of a mip-filtered 2D texture not kept on the host.
  uint32_t texture_quality_ = 0;
  void* watch_handle_ = nullptr;
  RenderDevice* device_ = nullptr;
  RenderCommandQueue* queue_ = nullptr;
  std::string miss_dump_directory_;
  // Debug: <files>/plume-skip holds a bit mask of draw kinds to drop (see IssueDraw).
  uint32_t skip_mask_ = 0;
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
  VkQueryPool timestamp_pool_ = VK_NULL_HANDLE;
  // per slot: two per draw
  VkQueryPool pair_pool_ = VK_NULL_HANDLE;
  bool trail_device_default_ = false;
  std::array<std::unique_ptr<RenderCommandList>, kPlumeFramesInFlight> lists_;
  uint32_t slot_ = kPlumeFramesInFlight - 1;
  // frame a pair was first recorded
  std::atomic<bool> device_watch_stop_{false};
  WatchThread device_watch_{device_watch_stop_};
  bool no_shadows_ = false, no_bloom_ = false;
  bool dedupe_descriptors_ = false;
  std::array<ResolvedLookup, kResolvedLookupSize> resolved_lookup_{};
  uint64_t resolved_lookup_serial_ = 1;
  uint64_t resolved_lookup_frame_ = UINT64_MAX;
  Stats stats_;
  uint64_t swaps_ = 0;

  // The subsystems.
  Pipelines pipelines{*this};
  Streams streams{*this};
  Profiling profiling{*this};
  Targets targets{*this};
  Textures textures{*this};
  Shaders shaders{*this};
  Lists lists{*this};
  Draw draw{*this};
  Recorder recorder{*this};
  HiZ hiz{*this};
  Census census{*this};
  Resolve resolve{*this};
  Patches patches{*this};
  Swap swap{*this};
  Reporting report{*this};
};


inline GpuBackend::GpuBackend(rex::memory::Memory* memory, rex::system::KernelState* kernel,
                        std::function<void(uint32_t, uint32_t)> dispatch_interrupt,
                        std::function<bool()> service_hardware)
      : memory_(memory),
        kernel_state_(kernel),
        dispatch_interrupt_(std::move(dispatch_interrupt)),
        service_hardware_(std::move(service_hardware)),
        worker_running_(true),
        wake_(rex::thread::Event::CreateAutoResetEvent(false)) {
    register_file_ = &registers_;
  }

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

inline RegisterFile* GpuBackend::registers() { return &registers_; }

inline Shader* GpuBackend::active_vertex_shader() const { return lists.active_vertex_shader_; }

inline Shader* GpuBackend::active_pixel_shader() const { return lists.active_pixel_shader_; }

inline uint32_t GpuBackend::ReadRegisterValue(uint32_t index) const {
    if (index < RegisterFile::kRegisterCount) return registers_.values[index];
    auto it = shaders.extended_register_values_.find(index);
    return it != shaders.extended_register_values_.end() ? it->second : 0;
  }

inline void Draw::MarkConstantsWritten(uint32_t first, uint32_t count) {
    // The two files are contiguous (vertex c0 at 0x4000, pixel c0 at 0x4400)
    // and so are the flags, one per 256 registers: the block is the offset
    // shifted. This runs for every constant register the guest writes.
    static_assert(XE_GPU_REG_SHADER_CONSTANT_256_X - XE_GPU_REG_SHADER_CONSTANT_000_X ==
                  kConstantChunks * (kConstantChunkSize / 4));
    constexpr uint32_t kShift = 8;
    static_assert((1u << kShift) == kConstantChunkSize / 4);
    constexpr uint32_t kRegisters = 2 * kConstantChunks << kShift;
    if (first <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31 &&
        uint64_t(first) + count > XE_GPU_REG_SHADER_CONSTANT_LOOP_00) {
      loop_constants_dirty_ = true;
    }
    const uint32_t lo = first - XE_GPU_REG_SHADER_CONSTANT_000_X;
    if (first < XE_GPU_REG_SHADER_CONSTANT_000_X || lo >= kRegisters || !count) return;
    const uint32_t hi = std::min(lo + count, kRegisters) - 1;
    bool* written = &constants_written_[0][0];
    for (uint32_t block = lo >> kShift; block <= hi >> kShift; ++block) written[block] = true;
  }

inline void Lists::StoreConstants(uint32_t first, const uint32_t* values, uint32_t count) {
    uint32_t* dst = &backend.register_file_->values[first];
    uint32_t lo = 0;
    while (lo < count && dst[lo] == values[lo]) ++lo;
    if (lo == count) return;
    uint32_t hi = count;
    while (dst[hi - 1] == values[hi - 1]) --hi;
    std::memcpy(dst + lo, values + lo, size_t(hi - lo) * sizeof(uint32_t));
    backend.draw.MarkConstantsWritten(first + lo, hi - lo);
  }

inline bool Lists::OnPredicatedSkip(uint32_t opcode) {
    if (backend.draw.trace_ && (opcode == xenos::PM4_DRAW_INDX || opcode == xenos::PM4_DRAW_INDX_2)) {
      std::fprintf(backend.draw.trace_, "predicated %s op=%X select=%llX mask=%llX\n", backend.swap.pred_pass_all_ ? "passed" : "skip",
                   opcode, (unsigned long long)bin_select_, (unsigned long long)bin_mask_);
    }
    return backend.swap.pred_pass_all_;
  }

inline void Lists::StoreBig(uint32_t physical, uint32_t value) {
    const uint32_t big = __builtin_bswap32(value);
    std::memcpy(backend.memory_->TranslatePhysical<uint8_t*>(physical), &big, 4);
  }

inline bool GpuBackend::NativeWorkPending() { return native_d3d::WorkPending(); }

inline void GpuBackend::RunNativeWork() {
    native_d3d::TakeWorkPending();
    if (!native_checked_) {
      swap.native_on_ = native_d3d::GetMode() == native_d3d::Mode::kNative;
      native_checked_ = true;
    }
    if (!swap.native_on_) return;
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
    return backend.swap.rec_enabled_ && !backend.targets.pair_capture_ && !backend.draw.sync_draws_ && sync_every_ <= 1 && !backend.draw.trace_;
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
    // Direct-mapped by address, valid for this frame and this serial: a map
    // that was emptied every frame was two hash lookups a sampled slot.
    ResolvedLookup& cached = backend.resolved_lookup_[(address >> 12) & (kResolvedLookupSize - 1)];
    if (cached.serial == backend.resolved_lookup_serial_ && cached.address == address) return cached.value;
    Resolved* latest = nullptr;
    for (auto& [key, r] : backend.targets.resolved_) {
      if (uint32_t(key) == address && r.texture && (!latest || r.written > latest->written)) latest = &r;
    }
    if (latest && latest->guest_bytes && latest->checked_frame != backend.swaps_) {
      latest->checked_frame = backend.swaps_;
      const uint8_t* memory = backend.memory_->TranslatePhysical<const uint8_t*>(address);
      if (ContentHash(memory, latest->guest_bytes) != latest->guest_hash) {
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
    cached = {address, backend.resolved_lookup_serial_, latest};
    if (backend.swap.resolve_log_ && latest) backend.resolve.LogResolveLine(fmt::format("f={} sampled {:08X}", backend.swaps_, address));
    return latest;
  }

inline void Targets::InvalidateResolvedLookups() { ++backend.resolved_lookup_serial_; }

// Debug dumps of a built pipeline (RENUT_PIPELINE_STATS, RENUT_PIPELINE_IR), in shaders.cpp.
void LogPipelineStatistics(RenderPipeline& pipeline, const HostShader* vs, const HostShader* ps);
void DumpPipelineInternals(RenderPipeline& pipeline, const HostShader* vs, const HostShader* ps,
                           const std::string& dir);

}  // namespace renut::gpu
