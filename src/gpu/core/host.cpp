// The app's GPU host (GpuHost) and the public API of plume_gpu.h.
#include "gpu/core/internal.h"
#include "d3d/frontend/native_d3d.h"
#include "platform/guest.h"
#include "guest/host_support.h"
#include "platform/log.h"
#include "platform/settings.h"

#include <rex/chrono.h>

#include <atomic>
#include <memory>
#include <mutex>

// ---------------------------------------------------------------------------
// The GPU host
// ---------------------------------------------------------------------------

// The GPU backend, run by the app itself: the SDK starts no graphics driver (GraphicsFamily::kNone, renut_app.cpp),
// so nothing of its graphics system is in the way. The backend's worker is a host thread of the title's kernel
// (host::StartHostThread), started when the game creates its Direct3D device (d3d/scene/startup.cpp) and stopped
// by the app once the game's threads are (RunSession), before the title unloads. Guest writes are watched through
// host::WriteWatch. The frame is presented by Plume, to the window the app hands it (PlumeRenderer::Start).
//
// There is no vertical blank thread and no graphics interrupt: the device's start does not register the game's
// interrupt handler (sub_826573F0), so its vblank handler (sub_826541C8) is done here, natively, when the clock is
// read: at the presents and the callback records (TickVblank).

namespace renut::gpu {

class GpuHost {
 public:
  bool Start() {
    refresh_hz_ = std::max(1.0, double(host::GetVideoMode().refresh_rate));
    backend_ = std::make_unique<GpuBackend>();
    backend_->dispatch_callback_ = [this](uint32_t function, uint32_t context, uint32_t cpu) {
      RunCallback(function, context, cpu);
    };
    backend_->tick_vblank_ = [this] { TickVblank(); };
    if (!backend_->Initialize()) {
      backend_.reset();
      return false;
    }
    return true;
  }

  void Stop() {
    if (backend_) {
      backend_->Shutdown();
      backend_.reset();
    }
    if (vblank_block_) {
      host::SystemFree(vblank_block_);
      vblank_block_ = 0;
    }
  }

 private:
  // A callback record of a list: function(context) on the backend's thread.
  void RunCallback(uint32_t function, uint32_t context, uint32_t) {
    if (!function) return;
    const uint64_t args[] = {context};
    host::RunGuest(function, args, 1);
  }

  // The game's vblank handler (sub_826541C8) for `vblanks` vertical blanks, on the device's words: the counter
  // (+16548) and its time stamp (+16552); the swaps queued behind a vblank (the queue at +16572, eight bytes an
  // entry, read index +16700, write index +16704), each counted as presented (+16568) once the counter reaches
  // its vblank; the game's vblank callback (+16544), given {counter, presented, 0}; and the time in the
  // timebase's units >> 10 (+21676). A queued swap with a front buffer is the hardware's to show (the display
  // register 0x6110): here it is only counted. The swaps are immediate and the title sets no vblank callback, so
  // in practice it is the counter and the stamps.
  void RunVblank(uint32_t device, uint32_t vblanks) {
    if (!device) return;
    auto word = [](uint32_t address) { return std::atomic_ref<uint32_t>(*host::Virtual<uint32_t*>(address)); };
    auto load = [&](uint32_t offset) { return __builtin_bswap32(word(device + offset).load()); };
    auto store = [&](uint32_t offset, uint32_t value) { word(device + offset).store(__builtin_bswap32(value)); };
    const uint32_t counter = load(16548) + vblanks;
    store(16548, counter);
    store(16552, uint32_t(rex::Clock::QueryGuestTickCount()));
    for (uint32_t guard = 0; load(16700) != load(16704) && guard < 16; ++guard) {
      const uint32_t entry = 16572 + (load(16700) & 0xF) * 8;
      if (load(entry + 4) > counter) break;
      store(16568, load(16568) + 1);
      if (!load(entry)) {
        if (const uint32_t writeback = load(10900)) word(writeback + 4).store(0);
      }
      store(23376, uint32_t(rex::Clock::QueryGuestTickCount()));
      store(16700, load(16700) + 1);
    }
    if (const uint32_t callback = load(16544)) {
      if (!vblank_block_) vblank_block_ = host::SystemAlloc(16, 16);
      if (vblank_block_) {
        word(vblank_block_).store(__builtin_bswap32(counter));
        word(vblank_block_ + 4).store(__builtin_bswap32(load(16568)));
        word(vblank_block_ + 8).store(0);
        const uint64_t args[] = {vblank_block_};
        host::RunGuest(callback, args, 1);
      }
    }
    store(21676, uint32_t(rex::Clock::QueryGuestTickCount() >> 10));
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
    RunVblank(native_d3d::MainDevice(), uint32_t(vblanks));
  }

  std::unique_ptr<GpuBackend> backend_;
  double refresh_hz_ = 60.0;
  uint32_t vblank_block_ = 0;
  pacing::Clock::time_point tick_origin_{};
};

}  // namespace renut::gpu

namespace renut {
using namespace gpu;

namespace {
std::mutex g_host_mutex;
std::unique_ptr<GpuHost> g_host;
}  // namespace

bool PlumeGpuStart() {
  std::lock_guard lock(g_host_mutex);
  if (g_host) return true;
  auto host = std::make_unique<GpuHost>();
  if (!host->Start()) {
    REXLOG_CAT_ERROR(rex::log::renut, "Renut: the GPU backend did not start");
    return false;
  }
  g_host = std::move(host);
  REXLOG_CAT_INFO(rex::log::renut, "Renut: GPU backend started with the game's Direct3D device");
  return true;
}

void PlumeGpuStop() {
  std::lock_guard lock(g_host_mutex);
  if (!g_host) return;
  g_host->Stop();
  g_host.reset();
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
