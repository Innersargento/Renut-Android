// The graphics driver of the app (PlumeGraphicsDriver) and the public API of plume_gpu.h.
#include "gpu/core/internal.h"
#include "platform/guest.h"
#include "guest/host_support.h"
#include "platform/log.h"
#include "platform/settings.h"

#include <rex/driver_context.h>
#include <rex/gpu/xenos_graphics_driver.h>
#include <rex/settings_registry.h>

// ---------------------------------------------------------------------------
// Graphics driver
// ---------------------------------------------------------------------------

// The GPU the app hosts, as the SDK's graphics system runs it: the guest's video hardware as the game's
// Direct3D and the kernel see it - the vertical blank and its interrupt, the GPU backend behind the
// native front end. The SDK's Xenos command processor has no part in it; the ring buffer exports reach
// this driver and are ignored (the front end of d3d/frontend records the lists itself), and the Vd
// exports that matter are overridden in kernel_overrides.cpp. The frame is presented by Plume itself, to the
// window the app hands it (PlumeRenderer::Start), so BeginPresent has nothing to do.

namespace renut::gpu {

class PlumeGraphicsDriver : public rex::gpu::IXenosGraphicsDriver {
 public:
  std::string_view GetName() const override { return "plume"; }

  rex::Result<void> Initialize(rex::IDriverContext& context) override {
    if (auto found = context.GetSettingsRegistry().Find("gpu.vsync")) {
      vsync_ = static_cast<rex::Setting<bool>*>(*found);
    }
    return rex::Ok();
  }

  void Shutdown() override { DetachTitle(); }

  rex::Result<void> AttachTitle(const rex::TitleInfo& info, rex::IPhysicalMemory&,
                                rex::IPhysicalWriteWatch& watch, rex::IGuestInterrupts& interrupts) override {
    interrupts_ = &interrupts;
    host::SetWriteWatch(&watch);
    host::SetVideoMode(info.video_mode);
    refresh_hz_ = std::max(1.0, double(info.video_mode.refresh_rate));
    backend_ = std::make_unique<GpuBackend>();
    backend_->dispatch_callback_ = [this](uint32_t function, uint32_t context, uint32_t cpu) {
      RunCallback(function, context, cpu);
    };
    backend_->tick_vblank_ = [this] { TickVblank(); };
    if (!backend_->Initialize(interrupts)) {
      backend_.reset();
      return rex::Err(rex::ErrorCategory::kRuntime, "the GPU backend did not start");
    }
    // The frame pacer makes every swap immediate when the game's own 30 FPS is paced too
    // (renut_pace_original_fps); then the vertical blank only has a clock to keep, which TickVblank
    // does at the swaps. Otherwise a swap can be queued behind a vblank and the thread must deliver it.
    stub_vsync_ = renut::GetSettings().pace_original_fps.Get();
    if (stub_vsync_) {
      REXLOG_CAT_INFO(rex::log::renut, "Renut: no GPU VSync thread (the vblank clock is kept at the swaps)");
      return rex::Ok();
    }
    vsync_running_ = true;
    auto started = interrupts.StartHostThread("GPU VSync", [this](std::stop_token) {
      renut::MarkHostThread();
      VblankClock();
    });
    if (!started) return rex::Err(rex::ErrorCategory::kRuntime, started.error().message);
    vsync_thread_ = std::move(*started);
    return rex::Ok();
  }

  void DetachTitle() override {
    vsync_running_ = false;
    vsync_thread_.Reset();
    if (backend_) {
      backend_->Shutdown();
      backend_.reset();
    }
    host::SetWriteWatch(nullptr);
    interrupts_ = nullptr;
  }

  // The game swapped since the last call, or the wait is over.
  rex::gpu::FrameStatus WaitForFrame(rex::u32 timeout_ms) override {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    const uint64_t seen = seen_swaps_;
    for (;;) {
      const uint64_t now = renut_guest_swap_count();
      if (now != seen) {
        seen_swaps_ = now;
        return {.new_frame = true};
      }
      if (std::chrono::steady_clock::now() >= deadline) return {};
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }

  void ResizeSurface(rex::u32, rex::u32) override {}
  bool BeginPresent() override { return false; }
  void EndPresent() override {}
  void RenderOverlays(ImDrawData&) override {}

  // The ring buffer and the register window: the game's front end does not use them any more (a session of
  // the native front end logged no access), so reads are 0 and writes are dropped.
  void InitializeRingBuffer(rex::u32, rex::u32) override {}
  void EnableReadPointerWriteBack(rex::u32, rex::u32) override {}
  rex::u32 ReadRegister(rex::u32 index) override {
    NoteRegister(index, false, 0);
    return 0;
  }
  void WriteRegister(rex::u32 index, rex::u32 value) override { NoteRegister(index, true, value); }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) {
    interrupt_callback_data_ = user_data;
    REXLOG_CAT_INFO(rex::log::renut, "SetInterruptCallback({:08X}, {:08X})", callback, user_data);
  }

 private:
  // Runs the guest's interrupt callback on the calling thread, as the console's GPU interrupt would on the
  // CPU it names.
  void DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
    if (!interrupts_) return;
    if (cpu == 0xFFFFFFFF) cpu = 2;
    (void)interrupts_->RaiseGraphicsInterrupt(source, cpu);
  }

  // A callback record of a list: function(context) on the backend's thread.
  void RunCallback(uint32_t function, uint32_t context, uint32_t) {
    if (!function) return;
    const uint64_t args[] = {context};
    host::RunGuest(function, args, 1);
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
      return reinterpret_cast<uint32_t*>(host::Virtual<uint8_t*>(interrupt_callback_data_ + offset));
    };
    auto load = [&](uint32_t offset) { return __builtin_bswap32(std::atomic_ref<uint32_t>(*word(offset)).load()); };
    auto store = [&](uint32_t offset, uint32_t value) {
      std::atomic_ref<uint32_t>(*word(offset)).store(__builtin_bswap32(value));
    };
    if (load(16700) != load(16704) || load(16544) != 0) return false;
    const uint64_t time = rex::Clock::QueryGuestTickCount();
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
    const auto now = Clock::now();
    if (!tick_origin_.time_since_epoch().count()) {
      tick_origin_ = now;
      return;
    }
    const double scale = std::max(0.001, rex::Clock::GetGuestTimeScalar());
    const auto period = std::chrono::nanoseconds(int64_t(1e9 / refresh_hz_ / scale));
    const int64_t elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - tick_origin_).count();
    const int64_t vblanks = elapsed / std::max<int64_t>(1, period.count());
    if (vblanks <= 0) return;
    tick_origin_ += period * vblanks;
    if (backend_) backend_->add_counter(uint32_t(vblanks));
    if (!RunVblankNatively(uint32_t(vblanks))) DispatchInterruptCallback(0, 2);
  }

  void MarkVblank() {
    // The counter the game sees move, then the interrupt that runs its handler.
    if (backend_) backend_->increment_counter();
    if (pacing::ImmediatePresentation() && RunVblankNatively()) return;
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
  void VblankClock() {
    using pacing::Clock;
    const double hz = refresh_hz_;
    auto next = Clock::now();
    bool first = true;
    while (vsync_running_) {
      // Host time per vblank: a guest period stretched by the guest clock's scale.
      const double scale = std::max(0.001, rex::Clock::GetGuestTimeScalar());
      const double seconds = (!vsync_ || vsync_->Get()) ? 1.0 / hz : 0.001;
      const auto period = std::chrono::nanoseconds(int64_t(seconds / scale * 1e9));
      if (first) {
        next += period;
        first = false;
      }
      // Waits in slices so that a long period (paused) still notices the stop.
      for (;;) {
        if (!vsync_running_) return;
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
  }

  // The first read and the first write of each register are logged, to say so if something reaches them.
  void NoteRegister(uint32_t r, bool write, uint32_t value) {
    if (r >= seen_registers_.size()) return;
    std::atomic<uint8_t>& seen = seen_registers_[r];
    const uint8_t bit = write ? 2 : 1;
    if (seen.load(std::memory_order_relaxed) & bit) return;
    if (seen.fetch_or(bit, std::memory_order_relaxed) & bit) return;
    REXLOG_CAT_INFO(rex::log::renut, "Renut: MMIO register {:04X} first {}{}", r, write ? "written, value " : "read",
                write ? std::format("{:08X}", value) : std::string());
  }

  std::array<std::atomic<uint8_t>, 0x4000> seen_registers_{};
  rex::IGuestInterrupts* interrupts_ = nullptr;
  rex::Setting<bool>* vsync_ = nullptr;
  std::unique_ptr<GpuBackend> backend_;
  uint32_t interrupt_callback_data_ = 0;
  double refresh_hz_ = 60.0;
  uint64_t seen_swaps_ = 0;
  std::atomic<bool> vsync_running_{false};
  bool stub_vsync_ = false;  // no thread: TickVblank at the swaps
  pacing::Clock::time_point tick_origin_{};
  rex::ScopeGuard vsync_thread_;
};

}  // namespace renut::gpu

namespace renut {
using namespace gpu;

// The driver in use (plume_gpu.h).
namespace {
std::atomic<PlumeGraphicsDriver*> g_driver{nullptr};
}  // namespace

std::unique_ptr<rex::IDriver> CreatePlumeGraphicsDriver() {
  struct Owned final : PlumeGraphicsDriver {
    Owned() { g_driver.store(this); }
    ~Owned() override { g_driver.store(nullptr); }
  };
  return std::make_unique<Owned>();
}

bool PlumeGpuHosted() { return g_driver.load() != nullptr; }

void PlumeGpuSetInterruptCallback(uint32_t callback, uint32_t user_data) {
  if (auto* driver = g_driver.load()) driver->SetInterruptCallback(callback, user_data);
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
    REXLOG_CAT_INFO(rex::log::renut, "Renut: scene {:08X} (asset {:08X}, ref {:08X}) requested; none of its pipelines known yet", scene,
                asset, ref);
  } else {
    REXLOG_CAT_INFO(rex::log::renut, "Renut: scene {:08X} (asset {:08X}, ref {:08X}) requested; {} of its {} pipelines left to build", scene,
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
    REXLOG_CAT_WARN(rex::log::renut, "Renut: scene {:08X}: its pipelines stopped with {} of {} left; it opens without them",
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
  REXLOG_CAT_INFO(rex::log::renut, "Renut: scene {:08X} opens {} ms after its request{}; {} of its pipelines were known",
              p.scene.load(std::memory_order_relaxed),
              std::chrono::duration_cast<std::chrono::milliseconds>(now - p.scene_asked).count(),
              held ? ", its loading screen held for the pipelines" : "", p.scene_total);
}

}  // namespace renut
