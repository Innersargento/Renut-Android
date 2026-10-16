// The report written to files/native-d3d.txt.
#include "internal.h"

namespace renut::native_d3d {

// The report.

namespace {
std::mutex g_report_mutex;
std::string g_report_path;
}  // namespace

void NativeReport(uint64_t frame, uint64_t draws, uint64_t bad,
                  const std::function<std::string()>& executor) {
  static uint64_t last_frame = 0, last_draws = 0;
  if (frame % 30 == 0) {
    // The live switch for other devices' immediate calls.
    std::string path;
    {
      std::lock_guard lock(g_report_mutex);
      path = g_report_path;
    }
    path = path.substr(0, path.rfind('/')) + "/native-d3d-foreign";
    if (FILE* file = std::fopen(path.c_str(), "r")) {
      int mode = 1;
      if (std::fscanf(file, "%d", &mode) == 1 && mode >= 0 && mode <= 2 &&
          mode != g_foreign_mode.exchange(mode, std::memory_order_relaxed)) {
        REXLOG_INFO("Renut: native Direct3D: other devices' immediate calls in mode {}", mode);
      }
      std::fclose(file);
    }
  }
  if (frame < last_frame + 300) return;
  const double f = double(frame - last_frame);
  auto c = [](const std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); };
  std::string out = fmt::format(
      "native Direct3D lists - frames {}..{}\n\n== per frame: draws {:.1f}; "
      "problems so far {}\n",
      last_frame, frame, (draws - last_draws) / f, bad);
  out += fmt::format(
      "== front end (total): draws {}, flushes {}, shader loads {}, patched loads {}, begin vertices {}, "
      "runs {} (unknown {}), recordings {}, recording devices {}, immediate calls on another device {}, publishes {}, "
      "screen extent {}, predicated tiling {}, SetPending_Predicated {}, no scratch {}\n",
      c(g_counts.draws), c(g_counts.flushes), c(g_counts.shader_loads), c(g_counts.patched_loads),
      c(g_counts.begin_vertices), c(g_counts.runs), c(g_counts.unknown_runs),
      c(g_counts.recordings), c(g_counts.devices), c(g_counts.foreign_immediate), c(g_counts.publishes),
      c(g_counts.screen_extent), c(g_counts.predicated_tiling), c(g_counts.set_pending_predicated),
      c(g_counts.no_scratch));
  out += fmt::format(
      "== sync (total): {} doorbells ({} with native sync), fences {}, presents {}, callbacks {}, lists {} "
      "published / {} run, stuck waits {}; BeginIndexedVertices {}\n",
      c(g_counts.doorbells), c(g_counts.native_doorbells), c(g_counts.fences), c(g_counts.presents),
      c(g_counts.callbacks), c(g_counts.lists_published), c(g_counts.lists_executed), c(g_counts.stuck_waits),
      c(g_counts.indexed_up));
  {
    const uint64_t clip = g_foreign_clip.load(std::memory_order_relaxed);
    out += fmt::format(
        "== other devices' immediate calls (mode {}): last device {:08X}, doorbells {}, last surface clip "
        "({},{})-({},{}); per frame by function:",
        g_foreign_mode.load(), g_foreign_device.load(), c(g_foreign_doorbells), clip >> 48, (clip >> 32) & 0xFFFF,
        (clip >> 16) & 0xFFFF, clip & 0xFFFF);
    static std::array<uint64_t, renut::d3d::kFunctionCount> last{};
    for (uint32_t i = 0; i < g_foreign.size(); ++i) {
      const uint64_t n = c(g_foreign[i]);
      if (n != last[i]) out += fmt::format(" {} {:.1f};", kFunctionNames[i], double(n - last[i]) / f);
      last[i] = n;
    }
    out += "\n";
  }
  out += fmt::format("== sync records: {} structured effect records, {} of {} doorbells without a ring packet, "
                     "worker runs {}\n",
                     c(g_counts.effect_records), c(g_counts.doorbells_skipped), c(g_counts.doorbells),
                     c(g_counts.worker_runs));
  if (executor) out += executor();
  last_frame = frame;
  last_draws = draws;
  std::string path;
  {
    std::lock_guard lock(g_report_mutex);
    path = g_report_path;
  }
  if (!path.empty()) {
    if (FILE* file = std::fopen(path.c_str(), "w")) {
      std::fwrite(out.data(), 1, out.size(), file);
      std::fclose(file);
    }
  }
}

bool RecordPresent(uint32_t frontbuffer, uint32_t width, uint32_t height) {
  std::lock_guard lock(g_front.mutex);
  if (!g_front.sync_recording || !g_front.main_device) return false;
  uint8_t* base = rex::system::kernel_memory() ? rex::system::kernel_memory()->virtual_membase() : nullptr;
  if (!base) return false;
  const uint32_t dev = g_front.main_device, block = U32(base, dev + 10896);
  if (!block) return false;
  // D3DDevice_Swap stamps two slots of its eight-entry history (+21628 the
  // next, +21624 the oldest the vertical blank has not taken) with the GPU's
  // vblank counter, one just before VdSwap and one after, while fewer than
  // six are outstanding.
  const uint32_t next = U32(base, dev + 21628);
  const bool stamped = next - U32(base, dev + 21624) < 6;
  List& list = *g_front.immediate;
  list.words.push_back(kOpPresent);
  list.words.push_back(frontbuffer);
  list.words.push_back(width);
  list.words.push_back(height);
  list.words.push_back(stamped ? Phys(block + (16 + (next & 7)) * 4) : 0);
  list.words.push_back(stamped ? Phys(block + (16 + ((next + 1) & 7)) * 4) : 0);
  g_counts.presents.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void NoteListExecuted() { g_counts.lists_executed.fetch_add(1, std::memory_order_relaxed); }

void SetExecutorBusy(bool busy) { g_executor_busy.fetch_add(busy ? 1 : -1, std::memory_order_relaxed); }

void SetWake(std::function<void()> wake) {
  std::lock_guard lock(g_wake_mutex);
  g_wake = wake ? std::make_shared<std::function<void()>>(std::move(wake)) : nullptr;
}

bool WorkPending() { return g_work_pending.load(std::memory_order_acquire); }

void TakeWorkPending() {
  g_work_pending.store(false, std::memory_order_release);
  g_counts.worker_runs.fetch_add(1, std::memory_order_relaxed);
}

void SetFilesDirectory(std::string directory) {
  std::lock_guard lock(g_report_mutex);
  g_report_path = directory + "/native-d3d.txt";
}

}  // namespace renut::native_d3d
