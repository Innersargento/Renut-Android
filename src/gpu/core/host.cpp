// The graphics driver of the app (PlumeGraphicsDriver) and the public API of plume_gpu.h.
#include "gpu/core/internal.h"
#include "d3d/frontend/native_d3d.h"
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

// The GPU the app hosts, as the SDK's graphics system runs it. The graphics driver slot is how RexGlue hands
// a title's physical-memory write watch and its host threads to the app (driver-context.md), which the GPU
// backend needs; the rest of the Xenos driver contract has nothing to do here. The SDK's command processor
// has no part in it: the ring buffer and register calls are ignored (the native front end of d3d/frontend
// records the lists itself), and the frame is presented by Plume, to the window the app hands it
// (PlumeRenderer::Start), so BeginPresent has nothing to do.
//
// There is no vertical blank thread. The game's swaps never wait for one (every frame-rate target is held by
// the Pacer with an immediate presentation interval, frame_pacing.h), so the vblank only has a clock to keep:
// the counter the backend and the device read, brought up to date at the swaps and the callback records
// (TickVblank), which are the only moments it is read.

namespace renut::gpu {

class PlumeGraphicsDriver : public rex::gpu::IXenosGraphicsDriver {
 public:
  std::string_view GetName() const override { return "plume"; }

  rex::Result<void> Initialize(rex::IDriverContext&) override { return rex::Ok(); }

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
    return rex::Ok();
  }

  void DetachTitle() override {
    if (backend_) {
      backend_->Shutdown();
      backend_.reset();
    }
    host::SetWriteWatch(nullptr);
    interrupts_ = nullptr;
  }

  // The game swapped since the last call, or the wait is over. The SDK's present thread counts a frame for
  // each true answer (GetFrameStats).
  rex::gpu::FrameStatus WaitForFrame(rex::u32 timeout_ms) override {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
      const uint64_t now = renut_guest_swap_count();
      if (now != seen_swaps_) {
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

  // The ring buffer and the register window: the game's front end does not use them, so reads are 0 and
  // writes are dropped.
  void InitializeRingBuffer(rex::u32, rex::u32) override {}
  void EnableReadPointerWriteBack(rex::u32, rex::u32) override {}
  rex::u32 ReadRegister(rex::u32) override { return 0; }
  void WriteRegister(rex::u32, rex::u32) override {}

 private:
  // A callback record of a list: function(context) on the backend's thread.
  void RunCallback(uint32_t function, uint32_t context, uint32_t) {
    if (!function) return;
    const uint64_t args[] = {context};
    host::RunGuest(function, args, 1);
  }

  // The vblank handler (sub_826541C8, source 0 of the graphics interrupt sub_826573F0) with nothing for it to
  // do besides the device's clock: it adds the vblanks to the counter (+16548), stamps the time (+16552, and
  // +21676 in milliseconds-ish units, timebase >> 10), presents the swaps queued behind a vblank (the queue
  // at +16572, read index +16700, write index +16704) and calls the game's vblank callback (+16544). The swaps
  // are immediate and queue nothing, and nothing in the title sets a vblank callback, so the counter and the
  // stamps are what is left. False when the queue is not empty or a callback is set: then the game's handler
  // runs, through the interrupt callback the kernel keeps.
  bool RunVblankNatively(uint32_t device, uint32_t vblanks) {
    if (!device) return false;
    auto word = [&](uint32_t offset) {
      return reinterpret_cast<uint32_t*>(host::Virtual<uint8_t*>(device + offset));
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

  // The vblanks since the last call, by the clock of the video mode (the guest's, which crawls while the game
  // is paused), added to the backend's counter and to the device's words. Called on the GPU backend's thread
  // (a kernel thread) when a present or a callback record is reached.
  void TickVblank() {
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
    if (!RunVblankNatively(native_d3d::MainDevice(), uint32_t(vblanks)) && interrupts_)
      (void)interrupts_->RaiseGraphicsInterrupt(0, 2);
  }

  rex::IGuestInterrupts* interrupts_ = nullptr;
  std::unique_ptr<GpuBackend> backend_;
  double refresh_hz_ = 60.0;
  uint64_t seen_swaps_ = 0;
  pacing::Clock::time_point tick_origin_{};
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
