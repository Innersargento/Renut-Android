#include "adpf.h"

#include <dirent.h>
#include <pthread.h>
#include <sys/resource.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(renut_adpf, true, "reNut/Performance",
                    "Report each frame's work to Android's performance hint session (ADPF) so the system "
                    "holds the CPU at the lowest clocks that keep the game's pace");

REXCVAR_DECLARE(bool, renut_plume_uncap_fps);

// The kernel's swap path calls this when set (command_processor.cpp).
extern "C" void (*renut_frame_hook)();

namespace renut::adpf {
namespace {

// NDK types, opaque. The entry points are looked up at run time: the app still runs on
// API 28, where none of this exists.
struct Manager;
struct Session;
struct ThermalManager;

struct Api {
  Manager* (*get_manager)() = nullptr;
  Session* (*create_session)(Manager*, const int32_t*, size_t, int64_t) = nullptr;
  int64_t (*preferred_rate)(Manager*) = nullptr;
  int (*update_target)(Session*, int64_t) = nullptr;
  int (*report_actual)(Session*, int64_t) = nullptr;
  void (*close_session)(Session*) = nullptr;
  int (*prefer_power)(Session*, bool) = nullptr;  // API 35
  ThermalManager* (*thermal_acquire)() = nullptr;
  float (*thermal_headroom)(ThermalManager*, int) = nullptr;
};

constexpr size_t kMaxThreads = 6;
// A thread must have used this share of a core over the scan window to count as carrying the game.
constexpr double kBusyShare = 0.08;
constexpr int64_t kScanSeconds = 2;

Api g_api;
Manager* g_manager = nullptr;
ThermalManager* g_thermal = nullptr;
int64_t g_target_ns = 33'333'333;
// The system names how often it wants reports (a device may say 160 ms, far above a frame).
// Frames in between are folded into their longest, so a spike is never averaged away.
int64_t g_report_interval_ns = 0;
int64_t g_pending_max_ns = 0;
std::chrono::steady_clock::time_point g_last_report;

std::mutex g_mutex;  // guards the session, the thread table and the counters below
Session* g_session = nullptr;
struct Tracked {
  int32_t tid = 0;
  int fd = -1;            // /proc/self/task/<tid>/schedstat, kept open
  uint64_t last_run = 0;  // on-CPU nanoseconds at the previous frame
};
std::vector<Tracked> g_threads;
std::chrono::steady_clock::time_point g_last_frame;

// Frames and their work since the last log line.
uint64_t g_frames = 0, g_work_sum = 0, g_work_max = 0, g_over = 0;

std::atomic<bool> g_started{false}, g_active{true}, g_stop{false};
std::thread g_thread;
std::mutex g_wake_mutex;
std::condition_variable g_wake;

uint64_t ReadRunNs(int fd) {
  if (fd < 0) return 0;
  char text[96];
  const ssize_t n = pread(fd, text, sizeof(text) - 1, 0);
  if (n <= 0) return 0;
  text[n] = 0;
  return std::strtoull(text, nullptr, 10);  // first field: time on a CPU, ns
}

void CloseThreads() {
  for (Tracked& t : g_threads) {
    if (t.fd >= 0) close(t.fd);
  }
  g_threads.clear();
}

void CloseSessionLocked() {
  if (g_session && g_api.close_session) g_api.close_session(g_session);
  g_session = nullptr;
  CloseThreads();
}

// The threads that did the most work over the last window, busiest first. The audio and
// system threads are left out: they have their own priorities and a hint would only
// raise clocks for work that is not the frame.
struct Candidate {
  int32_t tid;
  uint64_t run_ns;
};
std::vector<Candidate> ScanThreads() {
  std::vector<Candidate> found;
  DIR* dir = opendir("/proc/self/task");
  if (!dir) return found;
  while (const dirent* entry = readdir(dir)) {
    const int32_t tid = std::atoi(entry->d_name);
    if (tid <= 0) continue;
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/schedstat", tid);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) continue;
    found.push_back({tid, ReadRunNs(fd)});
    close(fd);
  }
  closedir(dir);
  return found;
}

bool IgnoredThread(int32_t tid) {
  char path[64], name[32] = {};
  std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return true;
  const ssize_t n = read(fd, name, sizeof(name) - 1);
  close(fd);
  if (n <= 0) return true;
  const std::string comm(name);
  for (const char* skip : {"Audio", "AAudio", "XMA", "SDL", "binder", "Signal", "HeapTask", "FinalizerD",
                           "ReferenceQ", "Jit thread", "renut-adpf", "RenderThread", "hwuiTask", "mali-", "kgsl"}) {
    if (comm.find(skip) != std::string::npos) return true;
  }
  return false;
}

// Makes a session over the threads that were busiest in the last window, replacing the old one.
void BuildSession(const std::vector<Candidate>& before, const std::vector<Candidate>& after, double seconds) {
  if (!g_manager || !g_api.create_session) return;
  std::vector<Candidate> busy;
  for (const Candidate& now : after) {
    uint64_t was = 0;
    for (const Candidate& old : before) {
      if (old.tid == now.tid) { was = old.run_ns; break; }
    }
    const uint64_t used = now.run_ns > was ? now.run_ns - was : 0;
    if (double(used) / 1e9 / seconds >= kBusyShare && !IgnoredThread(now.tid)) busy.push_back({now.tid, used});
  }
  std::sort(busy.begin(), busy.end(), [](const Candidate& a, const Candidate& b) { return a.run_ns > b.run_ns; });
  if (busy.size() > kMaxThreads) busy.resize(kMaxThreads);
  if (busy.empty()) return;

  std::vector<int32_t> ids;
  for (const Candidate& c : busy) ids.push_back(c.tid);
  std::lock_guard<std::mutex> lock(g_mutex);
  // Unchanged: keep the session (and its history in the system's governor).
  if (g_session && g_threads.size() == ids.size()) {
    bool same = true;
    for (size_t i = 0; i < ids.size(); ++i) {
      if (std::find_if(g_threads.begin(), g_threads.end(), [&](const Tracked& t) { return t.tid == ids[i]; }) ==
          g_threads.end()) {
        same = false;
        break;
      }
    }
    if (same) return;
  }
  CloseSessionLocked();
  Session* session = g_api.create_session(g_manager, ids.data(), ids.size(), g_target_ns);
  if (!session) {
    REXLOG_WARN("Renut: ADPF could not create a hint session for {} threads", ids.size());
    return;
  }
  g_session = session;
  for (const int32_t tid : ids) {
    Tracked t;
    t.tid = tid;
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/schedstat", tid);
    t.fd = open(path, O_RDONLY | O_CLOEXEC);
    t.last_run = ReadRunNs(t.fd);
    g_threads.push_back(t);
  }
  g_last_frame = std::chrono::steady_clock::time_point{};
  std::string list;
  for (const int32_t tid : ids) list += (list.empty() ? "" : ",") + std::to_string(tid);
  REXLOG_INFO("Renut: ADPF hint session over {} threads [{}], target {:.1f} ms per frame", ids.size(), list,
              double(g_target_ns) / 1e6);
}

void Maintain() {
  pthread_setname_np(pthread_self(), "renut-adpf");
  setpriority(PRIO_PROCESS, 0, 10);  // behind the game
  std::vector<Candidate> before = ScanThreads();
  auto scanned = std::chrono::steady_clock::now();
  auto last_log = scanned;
  int log_round = 0;
  while (!g_stop.load()) {
    {
      std::unique_lock<std::mutex> lock(g_wake_mutex);
      g_wake.wait_for(lock, std::chrono::seconds(kScanSeconds), [] { return g_stop.load(); });
    }
    if (g_stop.load()) break;
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - scanned).count();
    std::vector<Candidate> after = ScanThreads();
    scanned = now;
    // Re-pick the set every round while the game runs; a paused game has none worth hinting.
    if (REXCVAR_GET(renut_adpf) && g_active.load() && seconds > 0.5) {
      BuildSession(before, after, seconds);
    } else {
      std::lock_guard<std::mutex> lock(g_mutex);
      CloseSessionLocked();
    }
    before = std::move(after);

    if (now - last_log >= std::chrono::seconds(10)) {
      last_log = now;
      uint64_t frames, mean_us, max_us, over;
      {
        std::lock_guard<std::mutex> lock(g_mutex);
        frames = g_frames;
        mean_us = frames ? g_work_sum / frames / 1000 : 0;
        max_us = g_work_max / 1000;
        over = g_over;
        g_frames = g_work_sum = g_work_max = g_over = 0;
      }
      float headroom = -1.0f;
      if (g_thermal && g_api.thermal_headroom) headroom = g_api.thermal_headroom(g_thermal, 10);
      // Near the thermal limit the system may refuse a boost: ask for the efficient side of the trade.
      bool efficient = false;
      {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_session && g_api.prefer_power && headroom >= 0.f) {
          efficient = headroom >= 0.9f;
          g_api.prefer_power(g_session, efficient);
        }
      }
      ++log_round;
      REXLOG_INFO(
          "Renut: ADPF {} frames in 10 s, work mean {:.1f} ms max {:.1f} ms of {:.1f} ms, {} over the target; "
          "thermal headroom {:.2f}{}",
          frames, mean_us / 1000.0, max_us / 1000.0, double(g_target_ns) / 1e6, over, headroom,
          efficient ? " (power-efficient)" : "");
    }
  }
}

}  // namespace

void Start() {
  if (g_started.exchange(true)) return;
  const double target_fps = REXCVAR_GET(renut_plume_uncap_fps) ? 60.0 : 30.0;
  if (!REXCVAR_GET(renut_adpf)) {
    REXLOG_INFO("Renut: ADPF off (renut_adpf = false)");
    g_started = false;
    return;
  }
  void* lib = dlopen("libandroid.so", RTLD_NOW);
  auto sym = [&](const char* name) { return lib ? dlsym(lib, name) : nullptr; };
  g_api.get_manager = reinterpret_cast<decltype(g_api.get_manager)>(sym("APerformanceHint_getManager"));
  g_api.create_session = reinterpret_cast<decltype(g_api.create_session)>(sym("APerformanceHint_createSession"));
  g_api.preferred_rate =
      reinterpret_cast<decltype(g_api.preferred_rate)>(sym("APerformanceHint_getPreferredUpdateRateNanos"));
  g_api.update_target =
      reinterpret_cast<decltype(g_api.update_target)>(sym("APerformanceHint_updateTargetWorkDuration"));
  g_api.report_actual =
      reinterpret_cast<decltype(g_api.report_actual)>(sym("APerformanceHint_reportActualWorkDuration"));
  g_api.close_session = reinterpret_cast<decltype(g_api.close_session)>(sym("APerformanceHint_closeSession"));
  g_api.prefer_power =
      reinterpret_cast<decltype(g_api.prefer_power)>(sym("APerformanceHint_setPreferPowerEfficiency"));
  g_api.thermal_acquire = reinterpret_cast<decltype(g_api.thermal_acquire)>(sym("AThermal_acquireManager"));
  g_api.thermal_headroom =
      reinterpret_cast<decltype(g_api.thermal_headroom)>(sym("AThermal_getThermalHeadroom"));
  if (!g_api.get_manager || !g_api.create_session || !g_api.report_actual || !g_api.close_session) {
    REXLOG_INFO("Renut: ADPF is not available on this Android version (needs API 33)");
    g_started = false;
    return;
  }
  g_manager = g_api.get_manager();
  if (!g_manager) {
    REXLOG_INFO("Renut: ADPF: the device has no performance hint manager");
    g_started = false;
    return;
  }
  if (g_api.thermal_acquire) g_thermal = g_api.thermal_acquire();
  g_target_ns = int64_t(1e9 / std::max(1.0, target_fps));
  const int64_t rate = g_api.preferred_rate ? g_api.preferred_rate(g_manager) : 0;
  // Never slower than a few frames: the governor needs to see a load change quickly.
  g_report_interval_ns = rate > 0 ? std::min<int64_t>(rate, 100'000'000) : 0;
  REXLOG_INFO("Renut: ADPF available: target {:.1f} ms ({:.0f} FPS), preferred update interval {:.1f} ms, thermal {}",
              double(g_target_ns) / 1e6, target_fps, double(rate) / 1e6, g_thermal ? "yes" : "no");
  g_stop = false;
  renut_frame_hook = &OnFrame;
  g_thread = std::thread(Maintain);
}

void Stop() {
  if (!g_started.exchange(false)) return;
  renut_frame_hook = nullptr;
  g_stop = true;
  g_wake.notify_all();
  if (g_thread.joinable()) g_thread.join();
  std::lock_guard<std::mutex> lock(g_mutex);
  CloseSessionLocked();
}

void SetActive(bool active) {
  g_active = active;
  if (!active) {
    std::lock_guard<std::mutex> lock(g_mutex);
    CloseSessionLocked();
  }
  g_wake.notify_all();
}

void OnFrame() {
  if (!g_active.load(std::memory_order_relaxed)) return;
  std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
  if (!lock.owns_lock() || !g_session || g_threads.empty()) return;  // never wait in the swap path
  const auto now = std::chrono::steady_clock::now();
  // The longest any thread worked since the last frame is the frame's critical path.
  uint64_t longest = 0;
  for (Tracked& t : g_threads) {
    const uint64_t run = ReadRunNs(t.fd);
    if (run > t.last_run) longest = std::max(longest, run - t.last_run);
    t.last_run = run;
  }
  const bool first = g_last_frame == std::chrono::steady_clock::time_point{};
  const int64_t wall = first ? 0 : std::chrono::duration_cast<std::chrono::nanoseconds>(now - g_last_frame).count();
  g_last_frame = now;
  // Not a frame's work: the first after a (re)start, or a gap (a load, a pause).
  if (first || wall <= 0 || wall > 250'000'000) return;
  const int64_t actual = std::min<int64_t>(std::max<int64_t>(int64_t(longest), 500'000), wall);
  g_pending_max_ns = std::max(g_pending_max_ns, actual);
  if (g_report_interval_ns <= 0 || now - g_last_report >= std::chrono::nanoseconds(g_report_interval_ns)) {
    g_api.report_actual(g_session, g_pending_max_ns);
    g_pending_max_ns = 0;
    g_last_report = now;
  }
  ++g_frames;
  g_work_sum += uint64_t(actual);
  g_work_max = std::max<uint64_t>(g_work_max, uint64_t(actual));
  if (actual > g_target_ns) ++g_over;
}

}  // namespace renut::adpf
