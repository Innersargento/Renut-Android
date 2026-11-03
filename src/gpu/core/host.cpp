// PlumeGpu, the video hardware the app hosts, and the public API of plume_gpu.h.
#include "gpu/core/internal.h"

// ---------------------------------------------------------------------------
// Graphics system
// ---------------------------------------------------------------------------

// The GPU the app hosts: the guest's video hardware as the game's Direct3D and
// the kernel see it - the register window at 0x7FC80000, the vertical blank and
// its interrupt, the ring and the GPU backend behind it. The SDK has no
// part in it (RuntimeConfig::graphics is null); the three Vd exports that
// reach it are overridden in kernel_overrides.cpp.
REXCVAR_DEFINE_BOOL(renut_native_gpu_callbacks, true, "reNut",
                    "The GPU's callback interrupt (the one D3DDevice_InsertCallback raises) is served here: "
                    "the callback is called with its context and the CPU's bit in the writeback block is cleared, "
                    "without the recompiled interrupt handler. Off: the handler runs, as before");

REXCVAR_DEFINE_BOOL(renut_native_vblank, true, "reNut",
                    "The vertical blank (interrupt source 0) is served here when nothing waits on it: the device's "
                    "vblank counter and times move on without the recompiled interrupt handler. The handler still "
                    "runs when a swap is queued behind a vblank or the game set a vblank callback");

REXCVAR_DEFINE_BOOL(renut_stub_vsync_thread, true, "reNut",
                    "No \"GPU VSync\" thread: with the frame pacer holding the game (presentation always "
                    "immediate) nothing needs the emulated vertical blank to tick between swaps, so the vblank "
                    "counter and the device's clock words are brought up to date when a swap or a callback "
                    "reaches the GPU backend. Off: the thread ticks them 60 times a second, as before");
REXCVAR_DECLARE(bool, renut_pace_original_fps);

namespace renut::gpu {

class PlumeGpu {
 public:
  bool Setup(rex::runtime::FunctionDispatcher* dispatcher, rex::system::KernelState* kernel) {
    memory_ = dispatcher->memory();
    function_dispatcher_ = dispatcher;
    kernel_state_ = kernel;
    backend_ = std::make_unique<GpuBackend>(
        memory_, kernel_state_, [this](uint32_t source, uint32_t cpu) { DispatchInterruptCallback(source, cpu); },
        [this] { return ServiceRing(); });
    backend_->dispatch_callback_ = [this](uint32_t function, uint32_t context, uint32_t cpu) {
      RunCallback(function, context, cpu);
    };
    backend_->tick_vblank_ = [this] { TickVblank(); };
    if (!backend_->Initialize()) return false;
    // The GPU's registers are 0x7FC80000-0x7FCFFFFF; the game reads the display
    // counters there and writes the ring's write pointer.
    memory_->AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0x0000FFFF, this,
                                   reinterpret_cast<rex::runtime::MMIOReadCallback>(ReadRegisterThunk),
                                   reinterpret_cast<rex::runtime::MMIOWriteCallback>(WriteRegisterThunk));
    // The frame pacer makes every swap immediate when the game's own 30 FPS is paced too
    // (renut_pace_original_fps); then the vertical blank only has a clock to keep, which TickVblank
    // does at the swaps. Otherwise a swap can be queued behind a vblank and the thread must deliver it.
    stub_vsync_ = REXCVAR_GET(renut_stub_vsync_thread) && REXCVAR_GET(renut_pace_original_fps);
    if (stub_vsync_) {
      REXLOG_INFO("Renut: no GPU VSync thread (the vblank clock is kept at the swaps)");
      return true;
    }
    vsync_running_ = true;
    vsync_thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() { return VblankClock(); }));
    vsync_thread_->set_name("GPU VSync");
    vsync_thread_->Create();
    return true;
  }

  void Shutdown() {
    if (backend_) {
      backend_->Shutdown();
      backend_.reset();
    }
    if (vsync_thread_) {
      vsync_running_ = false;
      vsync_thread_->Wait(0, 0, 0, nullptr);
      vsync_thread_.reset();
    }
  }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) {
    interrupt_callback_ = callback;
    interrupt_callback_data_ = user_data;
    REXLOG_INFO("SetInterruptCallback({:08X}, {:08X})", callback, user_data);
  }

 private:
  // Runs the guest's interrupt callback on the calling thread, as the console's
  // GPU interrupt would on the CPU it names.
  void DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
    if (!interrupt_callback_) return;
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread) return;
    if (cpu == 0xFFFFFFFF) cpu = 2;
    thread->SetActiveCpu(cpu);
    if (source == 1 && REXCVAR_GET(renut_native_gpu_callbacks) && RunCallbackInterrupt(thread, cpu)) return;
    uint64_t args[] = {source, interrupt_callback_data_};
    function_dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args,
                                           rex::countof(args));
  }

  // A callback record of a list: function(context) on the backend's thread, as the CPU it names.
  void RunCallback(uint32_t function, uint32_t context, uint32_t cpu) {
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread || !function) return;
    thread->SetActiveCpu(cpu);
    uint64_t args[] = {context};
    function_dispatcher_->ExecuteInterrupt(thread->thread_state(), function, args, rex::countof(args));
  }

  // The graphics interrupt handler's source 1 (sub_826573F0), served natively. The GPU's scratch
  // registers 4 and 5 were written back to the device's writeback block (device + 10900) at +16 and
  // +20: the callback and its context. The handler calls the callback when there is one, then takes
  // its CPU's bit out of the mask at +0 (scratch register 0, which the GPU sets for the CPUs the
  // callback is for) under the device's lock at +10904; the bit is the only thing it changes there, so
  // an atomic clear does for the lock. False when the device or its block is not there (the handler runs).
  bool RunCallbackInterrupt(rex::system::XThread* thread, uint32_t cpu) {
    if (!interrupt_callback_data_ || cpu >= 6) return false;
    auto word = [&](uint32_t address) {
      return reinterpret_cast<uint32_t*>(memory_->TranslateVirtual<uint8_t*>(address));
    };
    const uint32_t* device_block = word(interrupt_callback_data_ + 10900);
    const uint32_t block = device_block ? __builtin_bswap32(*device_block) : 0;
    if (!block) return false;
    const uint32_t callback = __builtin_bswap32(*word(block + 16));
    if (callback) {
      uint64_t args[] = {__builtin_bswap32(*word(block + 20))};
      function_dispatcher_->ExecuteInterrupt(thread->thread_state(), callback, args, rex::countof(args));
    }
    auto mask = std::atomic_ref<uint32_t>(*word(block));
    uint32_t seen = mask.load(std::memory_order_relaxed);
    for (;;) {
      const uint32_t cleared = __builtin_bswap32((__builtin_bswap32(seen) & ~(1u << cpu)) & 0x3F);
      if (mask.compare_exchange_weak(seen, cleared, std::memory_order_acq_rel)) break;
    }
    return true;
  }

  // Called by the renderer's worker on every pass.
  bool ServiceRing() {
    const uint32_t write_index = ring_.write_index.load();
    if (write_index == 0xBAADF00D || ring_.read_index == write_index) return false;
    ring_.read_index = write_index;
    return true;
  }

  // The vblank handler (sub_826541C8, source 0 of sub_826573F0) with nothing for it to do besides the
  // device's clock: it adds one to the vblank counter (+16548), stamps the time (+16552, and
  // +21676 in milliseconds-ish units, timebase >> 10), presents the swaps queued behind a vblank
  // (the queue at +16572, read index +16700, write index +16704) and calls the game's vblank
  // callback (+16544). With the frame pacer holding the game its swaps are immediate and queue
  // nothing, and nothing in the title sets a vblank callback, so the counter and the stamps are what
  // is left. False when the queue is not empty or a callback is set (the handler runs).
  bool RunVblankNatively(uint32_t vblanks = 1) {
    if (!interrupt_callback_data_) return false;
    auto word = [&](uint32_t offset) {
      return reinterpret_cast<uint32_t*>(memory_->TranslateVirtual<uint8_t*>(interrupt_callback_data_ + offset));
    };
    auto load = [&](uint32_t offset) { return __builtin_bswap32(std::atomic_ref<uint32_t>(*word(offset)).load()); };
    auto store = [&](uint32_t offset, uint32_t value) {
      std::atomic_ref<uint32_t>(*word(offset)).store(__builtin_bswap32(value));
    };
    if (load(16700) != load(16704) || load(16544) != 0) return false;
    const uint64_t time = rex::chrono::Clock::QueryGuestTickCount();
    store(16548, load(16548) + vblanks);
    store(16552, uint32_t(time));
    store(21676, uint32_t(time >> 10));
    return true;
  }

  // Without the thread: the vblanks since the last call, by the clock of the video mode (the guest's, which
  // crawls while the game is paused), are added to the backend's counter and to the device's words. Called
  // on the GPU backend's thread when a present or a callback record is reached, which are the only
  // moments the counter or the words are read. A queue of swaps or a vblank callback cannot be here
  // (the presentation is immediate and nothing sets one), but if there is one the handler runs.
  void TickVblank() {
    if (!stub_vsync_) return;
    using pacing::Clock;
    if (!tick_hz_) {
      rex::system::X_VIDEO_MODE mode;
      rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
      tick_hz_ = std::max(1.0, double(float(mode.refresh_rate)));
    }
    const auto now = Clock::now();
    if (!tick_origin_.time_since_epoch().count()) {
      tick_origin_ = now;
      return;
    }
    const double scale = std::max(0.001, rex::chrono::Clock::guest_time_scalar());
    const auto period = std::chrono::nanoseconds(int64_t(1e9 / tick_hz_ / scale));
    const int64_t elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - tick_origin_).count();
    const int64_t vblanks = elapsed / std::max<int64_t>(1, period.count());
    if (vblanks <= 0) return;
    tick_origin_ += period * vblanks;
    if (backend_) backend_->add_counter(uint32_t(vblanks));
    if (!(REXCVAR_GET(renut_native_vblank) && RunVblankNatively(uint32_t(vblanks))))
      DispatchInterruptCallback(0, 2);
  }

  void MarkVblank() {
    // The counter the game sees move, then the interrupt that runs its handler.
    if (backend_) backend_->increment_counter();
    if (REXCVAR_GET(renut_native_vblank) && pacing::ImmediatePresentation() && RunVblankNatively()) return;
    DispatchInterruptCallback(0, 2);
  }

  // The guest's vertical blank: one MarkVblank per refresh of the video mode the
  // game set (60 Hz), which is what its presentation interval counts and so what
  // holds it at 30 FPS.
  //
  // The SDK's worker polled every millisecond and, when it found itself late,
  // marked every vblank it owed in a row: a wake-up that overslept (common on a
  // phone's timer slack and under load) came out as two vblanks back to back and
  // then a gap, which the game showed as alternating short and long frames. This
  // one sleeps to an absolute deadline, spins the last stretch, marks exactly one
  // vblank per deadline, and when it fell more than two periods behind (the
  // process was descheduled, the interrupt ran long) it forgets the debt rather
  // than paying it as a burst. The deadlines are in the guest's clock: while the
  // game is paused its time crawls, and so do the vblanks.
  int VblankClock() {
    using pacing::Clock;
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    const double hz = std::max(1.0, double(float(mode.refresh_rate)));
    auto next = Clock::now();
    bool first = true;
    while (vsync_running_) {
      // Host time per vblank: a guest period stretched by the guest clock's scale.
      const double scale = std::max(0.001, rex::chrono::Clock::guest_time_scalar());
      const double seconds = REXCVAR_GET(vsync) ? 1.0 / hz : 0.001;
      const auto period = std::chrono::nanoseconds(int64_t(seconds / scale * 1e9));
      if (first) {
        next += period;
        first = false;
      }
      // Waits in slices so that a long period (paused) still notices the stop.
      for (;;) {
        if (!vsync_running_) return 0;
        const auto now = Clock::now();
        if (now >= next) break;
        const auto remaining = next - now;
        if (remaining > pacing::kSpinTail * 2) {
          std::this_thread::sleep_for(
              std::min<Clock::duration>(remaining - pacing::kSpinTail, std::chrono::milliseconds(4)));
        } else {
          std::this_thread::yield();
        }
      }
      MarkVblank();
      next = pacing::NextTick(next, Clock::now(), period);
    }
    return 0;
  }

  static uint32_t ReadRegisterThunk(void*, PlumeGpu* gpu, uint32_t addr) { return gpu->ReadRegister(addr); }
  static void WriteRegisterThunk(void*, PlumeGpu* gpu, uint32_t addr, uint32_t value) {
    gpu->WriteRegister(addr, value);
  }

  uint32_t ReadRegister(uint32_t addr) {
    const uint32_t r = (addr & 0xFFFF) / 4;
    switch (r) {
      case 0x0F00:  // RB_EDRAM_TIMING
        return 0x08100748;
      case 0x0F01:  // RB_BC_CONTROL
        return 0x0000200E;
      case 0x194C: {  // R500_D1MODE_V_COUNTER
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      case 0x1951:  // interrupt status
        return 1;   // vblank
      case 0x1961: {  // AVIVO_D1MODE_VIEWPORT_SIZE: [width(0x0FFF), height(0x0FFF)]
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return (std::min(uint32_t(mode.display_width), uint32_t(0x0FFF)) << 16) |
               std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      default:
        break;
    }
    return backend_ && r < RegisterFile::kRegisterCount ? backend_->ReadRegisterValue(r) : 0;
  }

  void WriteRegister(uint32_t addr, uint32_t value) {
    const uint32_t r = (addr & 0xFFFF) / 4;
    if (r == 0x01C5) {  // CP_RB_WPTR
      ring_.write_index = value;
      if (backend_) backend_->Wake();
      return;
    }
    if (r == 0x1844) return;  // AVIVO_D1GRPH_PRIMARY_SURFACE_ADDRESS
    if (backend_ && r < RegisterFile::kRegisterCount) backend_->registers()->values[r] = value;
  }

  rex::memory::Memory* memory_ = nullptr;
  rex::runtime::FunctionDispatcher* function_dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  std::unique_ptr<GpuBackend> backend_;
  // The write pointer a device that still rings the doorbell moves (CP_RB_WPTR). The ring has no
  // contents: a pointer that moves is only work for the worker.
  struct Ring {
    uint32_t read_index = 0;  // the worker's thread only
    std::atomic<uint32_t> write_index{0};
  } ring_;
  uint32_t interrupt_callback_ = 0;
  uint32_t interrupt_callback_data_ = 0;
  std::atomic<bool> vsync_running_{false};
  bool stub_vsync_ = false;  // no thread: TickVblank at the swaps
  double tick_hz_ = 0;
  pacing::Clock::time_point tick_origin_{};
  rex::system::object_ref<rex::system::XHostThread> vsync_thread_;
};

}  // namespace renut::gpu

namespace renut {
using namespace gpu;

// The GPU hosted by the app (plume_gpu.h).
namespace {
std::unique_ptr<PlumeGpu> g_hosted_gpu;
}  // namespace

bool StartPlumeGpu(rex::runtime::FunctionDispatcher* dispatcher, rex::system::KernelState* kernel) {
  if (g_hosted_gpu) return true;
  auto gpu = std::make_unique<PlumeGpu>();
  if (!gpu->Setup(dispatcher, kernel)) {
    gpu->Shutdown();
    return false;
  }
  g_hosted_gpu = std::move(gpu);
  return true;
}

void StopPlumeGpu() {
  if (!g_hosted_gpu) return;
  g_hosted_gpu->Shutdown();
  g_hosted_gpu.reset();
}

bool PlumeGpuHosted() { return g_hosted_gpu != nullptr; }

void PlumeGpuSetInterruptCallback(uint32_t callback, uint32_t user_data) {
  if (g_hosted_gpu) g_hosted_gpu->SetInterruptCallback(callback, user_data);
}

void SetPlumeMissDumpDirectory(const std::string& directory) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  g_miss_dump_directory = directory;
}

void PlumeSceneRequested(uint32_t scene, uint32_t asset, uint32_t ref) {
  PrecompileState& p = g_precompile;
  p.scene.store(scene, std::memory_order_relaxed);
  std::lock_guard lock(p.mutex);
  // The scene's groups that are not built yet go to the front of the queue.
  p.scene_missing.clear();
  p.scene_total = p.scene_left = 0;
  const auto groups = p.scene_groups.find(scene);
  if (groups != p.scene_groups.end()) {
    for (const uint32_t g : groups->second) {
      p.scene_total += p.group_size[g];
      if (p.state[g] == PrecompileState::kDone) continue;
      p.scene_left += p.group_size[g];
      p.scene_missing.push_back(g);
      if (p.state[g] == PrecompileState::kPending) p.urgent.push_front(g);
    }
  }
  p.scene_asked = p.scene_moved = std::chrono::steady_clock::now();
  if (!p.predicting.load(std::memory_order_relaxed)) {
    p.scene_missing.clear();  // nothing builds them (low-memory mode)
  } else if (!p.scene_missing.empty()) {
    if (!p.running) {
      p.running = true;
      p.batch_start = p.scene_asked;
      p.batch_failed = 0;
    }
    p.changed.notify_all();
  }
  // Up until the scene starts whatever is known: the models it loads add
  // their predicted programs to it (PredictPipelines).
  p.loading = p.predicting.load(std::memory_order_relaxed) || !p.scene_missing.empty();
  if (groups == p.scene_groups.end()) {
    REXLOG_INFO("Renut: scene {:08X} (asset {:08X}, ref {:08X}) requested; none of its pipelines known yet", scene,
                asset, ref);
  } else {
    REXLOG_INFO("Renut: scene {:08X} (asset {:08X}, ref {:08X}) requested; {} of its {} pipelines left to build", scene,
                asset, ref, p.scene_missing.empty() ? 0 : p.scene_left, p.scene_total);
  }
}

void PlumePredictPipelines(std::vector<PlumeShaderPair> pairs) {
  if (pairs.empty() || !g_precompile.predicting.load(std::memory_order_relaxed)) return;
  std::lock_guard lock(g_predict.mutex);
  g_predict.queued.fetch_add(uint32_t(pairs.size()), std::memory_order_relaxed);
  g_predict.pairs.insert(g_predict.pairs.end(), pairs.begin(), pairs.end());
}

bool PlumeSceneReady() {
  // Models that loaded with the scene and whose programs are not queued yet.
  PrecompileState& p = g_precompile;
  if (g_predict.queued.load(std::memory_order_relaxed)) {
    std::lock_guard lock(p.mutex);
    if (std::chrono::steady_clock::now() - p.scene_asked < std::chrono::seconds(60)) return false;
  }
  std::lock_guard lock(p.mutex);
  if (p.scene_missing.empty()) return true;
  const size_t before = p.scene_missing.size();
  std::erase_if(p.scene_missing, [&](uint32_t g) {
    if (p.state[g] != PrecompileState::kDone) return false;
    p.scene_left -= p.group_size[g];
    return true;
  });
  const auto now = std::chrono::steady_clock::now();
  if (p.scene_missing.size() != before) p.scene_moved = now;
  // Gives up only if the building stops moving: a stuck driver must not hold
  // the game for ever.
  if (!p.scene_missing.empty() && p.running && now - p.scene_moved < std::chrono::seconds(60)) return false;
  if (!p.scene_missing.empty() && p.running) {
    REXLOG_WARN("Renut: scene {:08X}: its pipelines stopped with {} of {} left; it opens without them",
                p.scene.load(std::memory_order_relaxed), p.scene_left, p.scene_total);
  }
  p.scene_missing.clear();
  return true;
}

void PlumeSceneOpened(bool held) {
  PrecompileState& p = g_precompile;
  std::lock_guard lock(p.mutex);
  p.loading = false;
  const auto now = std::chrono::steady_clock::now();
  REXLOG_INFO("Renut: scene {:08X} opens {} ms after its request{}; {} of its pipelines were known",
              p.scene.load(std::memory_order_relaxed),
              std::chrono::duration_cast<std::chrono::milliseconds>(now - p.scene_asked).count(),
              held ? ", its loading screen held for the pipelines" : "", p.scene_total);
}

}  // namespace renut
