// The threads that make the game's sound, kept on time (see audio_threads.h).
#include "platform/audio_threads.h"

#include <dirent.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <rex/apu/audio_system.h>
#include <rex/ppc/context.h>
#include <rex/ppc/hook.h>
#include <rex/ppc/recompiled_code.h>

#include "platform/android_drivers.h"
#include "platform/log.h"
#include "platform/settings.h"

namespace renut::audio {
namespace {

constexpr int kAudioNice = -10;

// The cores whose top frequency is within 70% of the fastest: the big and prime cores. Empty (no pinning) when
// that is every core or fewer than two.
struct FastCores {
  cpu_set_t set;
  int chosen = 0, cores = 0;
};

const FastCores& Fast() {
  static const FastCores fast = [] {
    FastCores result;
    CPU_ZERO(&result.set);
    unsigned top[64] = {};
    unsigned fastest = 0;
    for (int cpu = 0; cpu < 64; ++cpu) {
      char path[96];
      std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
      FILE* file = std::fopen(path, "r");
      if (!file) break;
      if (std::fscanf(file, "%u", &top[cpu]) != 1) top[cpu] = 0;
      std::fclose(file);
      fastest = std::max(fastest, top[cpu]);
      result.cores = cpu + 1;
    }
    if (!fastest) return result;
    for (int cpu = 0; cpu < result.cores; ++cpu) {
      if (uint64_t(top[cpu]) * 10 >= uint64_t(fastest) * 7) {
        CPU_SET(cpu, &result.set);
        ++result.chosen;
      }
    }
    if (result.chosen < 2 || result.chosen == result.cores) result.chosen = 0;
    return result;
  }();
  return fast;
}

// Raises a thread of this process and keeps it on the fast cores.
void Raise(const char* what, pid_t tid) {
  const bool raised = setpriority(PRIO_PROCESS, id_t(tid), kAudioNice) == 0;
  const int raise_error = raised ? 0 : errno;
  const FastCores& fast = Fast();
  const bool pinned = fast.chosen && sched_setaffinity(tid, sizeof fast.set, &fast.set) == 0;
  if (raised) {
    REXLOG_CAT_INFO(rex::log::renut, "Renut: audio thread '{}' (tid {}) at nice {}{}", what, tid, kAudioNice,
                    pinned ? ", kept on the " + std::to_string(fast.chosen) + " fast cores of " + std::to_string(fast.cores)
                           : std::string());
  } else {
    REXLOG_CAT_WARN(rex::log::renut, "Renut: audio thread '{}' (tid {}) could not be raised: errno {}", what, tid,
                    raise_error);
  }
}

bool Enabled() { return renut::GetSettings().audio_threads_boost.Get(); }

// The calling thread, once.
void RaiseCurrent(const char* what) {
  thread_local bool done = false;
  if (done) return;
  done = true;
  if (Enabled()) Raise(what, gettid());
}

// A thread of the SDK found by the name it gave itself (the kernel keeps 15 characters of it).
void RaiseNamed(const char* name) {
  DIR* dir = opendir("/proc/self/task");
  if (!dir) return;
  const size_t length = std::strlen(name);
  while (dirent* entry = readdir(dir)) {
    const pid_t tid = pid_t(std::atoi(entry->d_name));
    if (tid <= 0) continue;
    char path[64];
    std::snprintf(path, sizeof path, "/proc/self/task/%d/comm", tid);
    FILE* file = std::fopen(path, "r");
    if (!file) continue;
    char comm[32] = {};
    const bool read = std::fgets(comm, sizeof comm, file) != nullptr;
    std::fclose(file);
    if (read && std::strncmp(comm, name, length) == 0) Raise(name, tid);
  }
  closedir(dir);
}

std::atomic<rex::apu::IAudioSystem*> g_system{nullptr};

// The underrun log, on the audio worker only.
struct UnderrunLog {
  std::chrono::steady_clock::time_point window, last_call;
  uint64_t base = 0;
  int32_t xruns = 0;
  bool started = false;
};
UnderrunLog g_log;

void NoteUnderruns() {
  rex::apu::IAudioSystem* system = g_system.load(std::memory_order_acquire);
  if (!system) return;
  const auto now = std::chrono::steady_clock::now();
  // The callback is not called while the game is paused or between engines; the underruns of such a gap are not
  // the mixer's, so a gap starts a new window.
  const bool gap = !g_log.started || now - g_log.last_call > std::chrono::seconds(1);
  g_log.last_call = now;
  if (!gap && now - g_log.window < std::chrono::seconds(10)) return;
  const rex::apu::AudioStats stats = system->GetStats();
  const android::AudioOutputStats output = android::AudioOutput();
  if (!gap && stats.underruns > g_log.base) {
    REXLOG_CAT_WARN(rex::log::renut,
                    "Renut: audio: {} periods without a mixed frame in the last {} s ({} frames queued, {} us from "
                    "submit to the device)",
                    stats.underruns - g_log.base,
                    std::chrono::duration_cast<std::chrono::seconds>(now - g_log.window).count(), stats.queued_frames,
                    stats.latency_us);
  }
  // The device's own underruns (its callback ran late): the clicks the queue above does not see. A stream opened
  // again (a route change) counts from 0.
  if (output.xruns < g_log.xruns) g_log.xruns = 0;
  if (output.xruns > g_log.xruns) {
    REXLOG_CAT_WARN(rex::log::renut, "Renut: audio: the device ran out {} times in the last {} s; its buffer is now {} frames",
                    output.xruns - g_log.xruns,
                    std::chrono::duration_cast<std::chrono::seconds>(now - g_log.window).count(), output.buffer_frames);
  }
  g_log.xruns = output.xruns;
  g_log.base = stats.underruns;
  g_log.window = now;
  g_log.started = true;
}

}  // namespace

void SetSystem(rex::apu::IAudioSystem* system) { g_system.store(system, std::memory_order_release); }

void OnRenderCallback() {
  static std::atomic<bool> sdk_threads{false};
  if (!sdk_threads.exchange(true) && Enabled()) {
    RaiseCurrent("Audio Worker");
    RaiseNamed("XMA Decoder");
  }
  NoteUnderruns();
}

}  // namespace renut::audio

// The game's mixer threads (made by its audio engine, sub_82664680, one per hardware thread it is given): the
// leader, whom the render callback wakes for a frame, and the workers it shares the frame with.
extern "C" void __imp__sub_82664508(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_826645D0(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_82664508) {
  renut::audio::RaiseCurrent("mixer");
  __imp__sub_82664508(ctx, base);
}
REX_HOOK_RAW(sub_826645D0) {
  renut::audio::RaiseCurrent("mixer worker");
  __imp__sub_826645D0(ctx, base);
}
