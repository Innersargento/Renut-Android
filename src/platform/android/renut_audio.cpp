// reNut's audio output for Android. See renut_audio.h.
//
// The path of one frame (256 samples x 6 channels, 48 kHz, 5.33 ms):
//
//   guest mixer --XAudioSubmitRenderDriverFrame--> SubmitRenderFrame
//       big-endian 5.1 -> little-endian stereo (NEON), written into a ring slot
//   AAudio data callback (the device's own real-time thread)
//       copies slots to the device buffer, nothing else: no lock, no allocation,
//       no conversion; returns one credit per slot it finished with
//   worker thread
//       spends a credit by running the title's render callback, which makes the
//       mixer produce the next frame
//
// Credits are the SDK's semaphore: a client starts with `queued_frames_` of
// them (cvar audio_maxqframes, default 8 = 43 ms), so the title is never more
// than that far ahead of what the device has played. The worker sleeps on one
// futex that every credit return bumps, instead of waiting on all the clients'
// handles at once.
#include "renut_audio.h"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <arm_neon.h>
#include <dirent.h>
#include <linux/futex.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>

#include <rex/audio/audio_driver.h>
#include <rex/audio/audio_system.h>
#include <rex/audio/downmix.h>
#include <rex/audio/flags.h>
#include <rex/audio/xma/decoder.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

#include "guest/host_support.h"

// The SDK's X_STATUS_* macros are written with the unqualified type.
using rex::X_STATUS;

namespace renut::audio {
namespace {

constexpr uint32_t kFrameSamples = 256;       // per channel, per guest frame
constexpr uint32_t kGuestChannels = 6;
constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kOutChannels = 2;
constexpr uint32_t kRingFrames = 64;          // power of two, >= the SDK's 64 queued frames
constexpr size_t kSlotCount = 8;              // the SDK's kMaximumClientCount
constexpr uint32_t kHandleTag = 0x41550000;   // 'AU' << 16, the handle the title keeps
constexpr uint32_t kErrorInvalidArg = 0x80070057;
constexpr uint32_t kStatusNoMemory = 0xC0000017;
constexpr int kAudioNice = -10;

std::atomic<uint64_t> g_silent_frames{0};
std::atomic<class RenutAudioSystem*> g_system{nullptr};

// Opening, underruns and errors go to logcat as well as the runtime log: the
// phone's runtime log is normally off, and these are what a report of crackling
// or silence needs first.
#define RENUT_AUDIO_LOG(level, ...) __android_log_print(ANDROID_LOG_##level, "renut-audio", __VA_ARGS__)

// Nice -10 (Android's own audio threads run at -16 and -19): above the game's
// threads and the pipeline compiles, so the scheduler gives these a core the
// moment they wake. Measured on the earlier SDL path under heavy compiling: the
// worker waited 100-158 us per wake for a core instead of ~35 us and the output
// filled the gaps with silence.
void RaiseThread(const char* what, pid_t tid) {
  if (setpriority(PRIO_PROCESS, static_cast<id_t>(tid), kAudioNice) == 0) {
    REXAPU_INFO("renut audio: thread '{}' (tid {}) at nice {}", what, tid, kAudioNice);
  } else {
    REXAPU_WARN("renut audio: could not raise thread '{}' (tid {}): errno {}", what, tid, errno);
  }
}

// The title's mixer costs 4-6 ms of CPU for every 5.33 ms frame it has to make:
// nearly all of a core, so whether it lands on a slow core decides whether the
// output keeps up. The scheduler places a thread that sleeps most of the time on
// the efficiency cores, where the same work takes half again as long. Pin the
// audio threads to the cores whose top frequency is within 70% of the fastest.
void PinToFastCores(const char* what, pid_t tid) {
  uint32_t top[CPU_SETSIZE] = {};
  uint32_t fastest = 0;
  int cores = 0;
  for (int cpu = 0; cpu < 32; ++cpu) {
    char path[96];
    std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
    FILE* file = std::fopen(path, "r");
    if (!file) break;
    unsigned value = 0;
    if (std::fscanf(file, "%u", &value) == 1) top[cpu] = value;
    std::fclose(file);
    fastest = std::max(fastest, top[cpu]);
    cores = cpu + 1;
  }
  if (!fastest) return;
  cpu_set_t set;
  CPU_ZERO(&set);
  int chosen = 0;
  for (int cpu = 0; cpu < cores; ++cpu) {
    if (top[cpu] >= fastest * 7 / 10) {
      CPU_SET(cpu, &set);
      ++chosen;
    }
  }
  if (chosen < 2 || chosen == cores) return;
  if (sched_setaffinity(tid, sizeof set, &set) == 0) {
    RENUT_AUDIO_LOG(INFO, "thread '%s' (tid %d) kept on the %d of %d cores at 70%% of the top frequency or more", what,
                    tid, chosen, cores);
  } else {
    RENUT_AUDIO_LOG(WARN, "could not pin thread '%s': errno %d", what, errno);
  }
}

// The thread that mixes: the one that signals what the render callback waits on
// (host_support.h). Called from that thread, on its first signal; it is at
// normal priority among the loading threads, and the callback's budget is its.
void MixerFound(int tid) {
  const std::string name = [&] {
    char path[64];
    std::snprintf(path, sizeof path, "/proc/self/task/%d/comm", tid);
    FILE* file = std::fopen(path, "r");
    char comm[32] = {};
    if (file) {
      std::fgets(comm, sizeof comm, file);
      std::fclose(file);
    }
    std::string text(comm);
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    return text;
  }();
  RENUT_AUDIO_LOG(INFO, "the mixer is guest thread '%s' (tid %d): priority raised, kept on the fast cores",
                  name.c_str(), tid);
  RaiseThread(name.c_str(), tid);
  PinToFastCores(name.c_str(), tid);
}

// The XMA decoder's thread belongs to the SDK; find it by the name it gave
// itself and raise it from outside.
bool RaiseThreadNamed(const char* name) {
  DIR* dir = opendir("/proc/self/task");
  if (!dir) return false;
  bool found = false;
  while (dirent* entry = readdir(dir)) {
    const pid_t tid = static_cast<pid_t>(std::atoi(entry->d_name));
    if (tid <= 0) continue;
    char path[64];
    std::snprintf(path, sizeof path, "/proc/self/task/%d/comm", tid);
    FILE* file = std::fopen(path, "r");
    if (!file) continue;
    char comm[32] = {};
    std::fgets(comm, sizeof comm, file);
    std::fclose(file);
    std::string text(comm);
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    // The kernel keeps the first 15 characters of a thread's name ('XMA Decoder (F8').
    if (text.compare(0, std::strlen(name), name) == 0) {
      RaiseThread(name, tid);
      PinToFastCores(name, tid);
      found = true;
    }
  }
  closedir(dir);
  return found;
}

void FutexWait(std::atomic<uint32_t>* address, uint32_t expected, std::chrono::milliseconds timeout) {
  timespec ts{};
  ts.tv_sec = static_cast<time_t>(timeout.count() / 1000);
  ts.tv_nsec = static_cast<long>((timeout.count() % 1000) * 1000000);
  syscall(SYS_futex, reinterpret_cast<uint32_t*>(address), FUTEX_WAIT_PRIVATE, expected, &ts, nullptr, 0);
}
void FutexWake(std::atomic<uint32_t>* address) {
  syscall(SYS_futex, reinterpret_cast<uint32_t*>(address), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
}
static_assert(sizeof(std::atomic<uint32_t>) == 4 && std::atomic<uint32_t>::is_always_lock_free);

// The guest's 5.1 render (channel-major, big-endian floats: fl fr fc lf bl br)
// folded to interleaved little-endian stereo. Same mix, order of operations and
// clamp as the SDK's reference (rex/audio/conversion.h), four samples at a time.
void FoldFrame(float* out, const float* in_be, const rex::audio::StereoFold& fold, float gain) {
  const float32x4_t center = vdupq_n_f32(fold.center);
  const float32x4_t lfe = vdupq_n_f32(fold.lfe);
  const float32x4_t surround = vdupq_n_f32(fold.surround);
  const float32x4_t scale = vdupq_n_f32(fold.scale * gain);
  const float32x4_t lo = vdupq_n_f32(-1.0f);
  const float32x4_t hi = vdupq_n_f32(1.0f);
  auto load = [&](uint32_t channel, uint32_t sample) {
    const uint8x16_t raw = vld1q_u8(reinterpret_cast<const uint8_t*>(in_be + channel * kFrameSamples + sample));
    return vreinterpretq_f32_u8(vrev32q_u8(raw));
  };
  for (uint32_t s = 0; s < kFrameSamples; s += 4) {
    const float32x4_t fl = load(0, s), fr = load(1, s), fc = load(2, s);
    const float32x4_t lf = load(3, s), bl = load(4, s), br = load(5, s);
    const float32x4_t mid = vaddq_f32(vmulq_f32(fc, center), vmulq_f32(lf, lfe));
    float32x4_t left = vmulq_f32(vaddq_f32(vaddq_f32(fl, mid), vmulq_f32(bl, surround)), scale);
    float32x4_t right = vmulq_f32(vaddq_f32(vaddq_f32(fr, mid), vmulq_f32(br, surround)), scale);
    left = vminq_f32(vmaxq_f32(left, lo), hi);
    right = vminq_f32(vmaxq_f32(right, lo), hi);
    float32x4x2_t pair;
    pair.val[0] = left;
    pair.val[1] = right;
    vst2q_f32(out + s * kOutChannels, pair);
  }
}

class RenutAudioSystem;

// One render client's output: an AAudio stream and the ring in front of it.
class AAudioDriver final : public rex::audio::AudioDriver {
 public:
  AAudioDriver(rex::memory::Memory* memory, RenutAudioSystem* owner, size_t slot)
      : AudioDriver(memory), owner_(owner), slot_(slot), ring_(new float[kRingFrames * kFrameSamples * kOutChannels]) {}
  ~AAudioDriver() override { Close(); }

  bool Open() {
    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || !builder) {
      RENUT_AUDIO_LOG(ERROR, "AAudio_createStreamBuilder failed");
      return false;
    }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder, kOutChannels);
    AAudioStreamBuilder_setSampleRate(builder, kSampleRate);
    AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_MUSIC);
    AAudioStreamBuilder_setDataCallback(builder, &AAudioDriver::DataCallback, this);
    AAudioStreamBuilder_setErrorCallback(builder, &AAudioDriver::ErrorCallback, this);
    const aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &stream_);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK || !stream_) {
      RENUT_AUDIO_LOG(ERROR, "openStream failed: %s", AAudio_convertResultToText(result));
      stream_ = nullptr;
      return false;
    }
    burst_ = std::max<int32_t>(AAudioStream_getFramesPerBurst(stream_), 1);
    capacity_ = AAudioStream_getBufferCapacityInFrames(stream_);
    // Four bursts (8 ms at a burst of 96): small next to the queue the title
    // keeps ahead, and enough that the device's own thread running late during
    // a load does not leave it with nothing to read (that is a click). Grown by
    // a burst, from the worker, whenever the device reports an underrun.
    buffer_frames_ = std::min<int32_t>(capacity_, burst_ * 4);
    const int32_t granted = AAudioStream_setBufferSizeInFrames(stream_, buffer_frames_);
    if (granted > 0) buffer_frames_ = granted;
    last_xruns_ = 0;
    RENUT_AUDIO_LOG(INFO, "client %zu open: %d Hz, %d ch, burst %d frames, buffer %d/%d frames (%.1f ms), "
                    "performance mode %d (1 = none, 12 = low latency), sharing %d, device %d",
                    slot_, AAudioStream_getSampleRate(stream_), AAudioStream_getChannelCount(stream_), burst_,
                    buffer_frames_, capacity_, 1000.0 * buffer_frames_ / AAudioStream_getSampleRate(stream_),
                    static_cast<int>(AAudioStream_getPerformanceMode(stream_)),
                    static_cast<int>(AAudioStream_getSharingMode(stream_)), AAudioStream_getDeviceId(stream_));
    return true;
  }

  void Close() {
    if (!stream_) return;
    AAudioStream_requestStop(stream_);
    AAudioStream_close(stream_);  // returns once the callback thread is gone
    stream_ = nullptr;
  }

  bool Start() {
    if (!stream_) return false;
    const aaudio_result_t result = AAudioStream_requestStart(stream_);
    if (result != AAUDIO_OK) {
      RENUT_AUDIO_LOG(ERROR, "requestStart failed: %s", AAudio_convertResultToText(result));
      return false;
    }
    playing_ = true;
    return true;
  }
  void Pause() {
    playing_ = false;
    if (stream_) AAudioStream_requestPause(stream_);
  }
  void Resume() {
    if (stream_ && !playing_) Start();
  }

  // The device went away (headphones, a route change): AAudio stops the stream
  // and reports it from its own thread, and the worker opens a new one.
  bool NeedsReopen() const { return disconnected_.load(std::memory_order_acquire); }
  void Reopen() {
    Close();
    disconnected_.store(false, std::memory_order_release);
    if (Open()) Start();
  }

  // Worker only: lets the buffer grow after an underrun, never shrink.
  void Tune() {
    if (!stream_ || buffer_frames_ >= capacity_) return;
    const int32_t xruns = AAudioStream_getXRunCount(stream_);
    if (xruns > last_xruns_) {
      last_xruns_ = xruns;
      const int32_t granted = AAudioStream_setBufferSizeInFrames(stream_, std::min(buffer_frames_ + burst_, capacity_));
      if (granted > 0) buffer_frames_ = granted;
      RENUT_AUDIO_LOG(INFO, "client %zu underrun %d, buffer now %d frames (%.1f ms)", slot_, xruns,
                      buffer_frames_, 1000.0 * buffer_frames_ / kSampleRate);
    }
  }

  // Starvation episodes since the last call: the title's mixer did not hand over
  // a frame in time. The worker answers them with a deeper queue.
  uint32_t TakeUnderruns() { return underrun_events_.exchange(0, std::memory_order_relaxed); }

  // The title's mixer thread, under the system's lock (one producer at a time).
  void SubmitFrame(uint32_t samples_ptr) override {
    const uint32_t head = head_.load(std::memory_order_relaxed);
    if (head - tail_.load(std::memory_order_acquire) >= kRingFrames) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const float gain = REXCVAR_GET(audio_mute) ? 0.0f : rex::audio::GetOutputGain();
    FoldFrame(ring_.get() + (head & (kRingFrames - 1)) * kFrameSamples * kOutChannels,
              memory_->TranslateVirtual<const float*>(samples_ptr), rex::audio::GetStereoFold(), gain);
    head_.store(head + 1, std::memory_order_release);
    got_frame_.store(true, std::memory_order_relaxed);
  }

 private:
  static aaudio_data_callback_result_t DataCallback(AAudioStream*, void* user, void* audio, int32_t frames) {
    static_cast<AAudioDriver*>(user)->Render(static_cast<float*>(audio), frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
  }
  static void ErrorCallback(AAudioStream*, void* user, aaudio_result_t error);

  // The device's real-time thread. Copies, counts, and tells the worker how
  // many slots it finished with.
  void Render(float* out, int32_t frames);

  RenutAudioSystem* owner_;
  size_t slot_;
  std::unique_ptr<float[]> ring_;  // kRingFrames stereo frames, 128 KB
  alignas(64) std::atomic<uint32_t> head_{0};  // next slot the producer writes
  alignas(64) std::atomic<uint32_t> tail_{0};  // next slot the callback reads
  uint32_t read_pos_ = 0;                      // samples already played of slot `tail_` (callback only)
  std::atomic<uint32_t> dropped_{0};
  std::atomic<bool> got_frame_{false};
  std::atomic<uint32_t> underrun_events_{0};
  // The callback's own: the last stereo sample played, and whether the output is
  // between a starvation and the first frame after it (see Render).
  float last_left_ = 0.0f, last_right_ = 0.0f;
  bool starved_ = false;
  std::atomic<bool> disconnected_{false};

  AAudioStream* stream_ = nullptr;
  bool playing_ = false;
  int32_t burst_ = 0, capacity_ = 0, buffer_frames_ = 0, last_xruns_ = 0;
};

// The slots of the render clients the title registered.
struct Slot {
  enum class State { kFree, kLive, kClosing } state = State::kFree;
  uint32_t callback = 0;
  uint32_t wrapped_arg = 0;                    // guest heap word holding callback_arg
  std::unique_ptr<AAudioDriver> driver;
  bool callback_running = false;               // worker is inside the title's callback
  std::atomic<uint32_t> credits{0};            // frames the title may still produce
  // The worker's own: how far ahead of the device the title is allowed to run
  // (credits at rest). It deepens when the output starves and settles back
  // when it has not for a while, by letting returned credits go unissued.
  uint32_t depth = 0, base_depth = 0, debt = 0;
  std::chrono::steady_clock::time_point last_starved{};
};

class RenutAudioSystem final : public rex::audio::AudioSystem {
 public:
  explicit RenutAudioSystem(rex::runtime::FunctionDispatcher* dispatcher) : AudioSystem(dispatcher) {
    g_system.store(this, std::memory_order_release);
  }
  ~RenutAudioSystem() override {
    Shutdown();
    g_system.store(nullptr, std::memory_order_release);
  }

  rex::X_STATUS Setup(rex::system::KernelState* kernel_state) override {
    rex::X_STATUS status = xma_decoder_->Setup(kernel_state);
    if (status) return status;
    worker_running_ = true;
    worker_thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_state, 128 * 1024, 0, [this]() {
          WorkerMain();
          return 0;
        }));
    worker_thread_->set_name("Audio Worker");
    worker_thread_->Create();
    return X_STATUS_SUCCESS;
  }

  void Shutdown() override {
    if (!worker_running_) return;
    xma_decoder_->Shutdown();  // its worker can stall in FFmpeg: stop it first
    worker_running_ = false;
    Wake();
    if (paused_) resume_event_->Set();
    if (worker_thread_) {
      // The worker may be inside a title callback blocked on guest objects, so
      // terminating is the last resort; give it a moment to unwind first.
      rex::thread::Thread* host_thread = worker_thread_->thread();
      const bool exited = host_thread &&
          rex::thread::Wait(host_thread, false, std::chrono::milliseconds(500)) == rex::thread::WaitResult::kSuccess;
      if (!exited) {
        REXAPU_WARN("renut audio: worker did not exit within 500 ms; terminating");
        worker_thread_->Terminate(0);
      }
      worker_thread_.reset();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (Slot& slot : slots_) {
      slot.driver.reset();
      if (slot.wrapped_arg) memory()->SystemHeapFree(slot.wrapped_arg);
      slot.wrapped_arg = 0;
      slot.state = Slot::State::kFree;
    }
  }

  // ---- what the XAudio exports call ----

  uint32_t Register(uint32_t callback_ptr, uint32_t driver_ptr) {
    if (!callback_ptr || !driver_ptr) return kErrorInvalidArg;
    const uint8_t* callback_words = memory()->TranslateVirtual<const uint8_t*>(callback_ptr);
    const uint32_t callback = rex::memory::load_and_swap<uint32_t>(callback_words);
    const uint32_t callback_arg = rex::memory::load_and_swap<uint32_t>(callback_words + 4);
    if (!callback) return kErrorInvalidArg;

    std::lock_guard<std::mutex> lock(mutex_);
    size_t index = kSlotCount;
    for (size_t i = 0; i < kSlotCount; ++i) {
      if (slots_[i].state == Slot::State::kFree) {
        index = i;
        break;
      }
    }
    if (index == kSlotCount) {
      REXAPU_WARN("renut audio: no free render client slot");
      return kStatusNoMemory;
    }
    const uint32_t wrapped = memory()->SystemHeapAlloc(4);
    if (!wrapped) return kStatusNoMemory;
    rex::memory::store_and_swap<uint32_t>(memory()->TranslateVirtual(wrapped), callback_arg);

    auto driver = std::make_unique<AAudioDriver>(memory(), this, index);
    if (!driver->Open() || !driver->Start()) {
      memory()->SystemHeapFree(wrapped);
      return static_cast<uint32_t>(X_STATUS_UNSUCCESSFUL);
    }
    Slot& slot = slots_[index];
    slot.callback = callback;
    slot.wrapped_arg = wrapped;
    slot.driver = std::move(driver);
    slot.credits.store(queued_frames_, std::memory_order_release);
    slot.depth = slot.base_depth = queued_frames_;
    slot.debt = 0;
    slot.last_starved = std::chrono::steady_clock::now();
    slot.state = Slot::State::kLive;
    rex::memory::store_and_swap<uint32_t>(memory()->TranslateVirtual(driver_ptr),
                                          kHandleTag | static_cast<uint32_t>(index));
    RENUT_AUDIO_LOG(INFO, "render client %zu registered (callback %08X, %u frames ahead)", index, callback,
                    queued_frames_);
    Wake();
    return 0;
  }

  uint32_t Unregister(uint32_t handle) {
    const size_t index = handle & 0xFFFF;
    std::unique_lock<std::mutex> lock(mutex_);
    if (index >= kSlotCount || slots_[index].state != Slot::State::kLive) return 0;
    Slot& slot = slots_[index];
    slot.state = Slot::State::kClosing;
    slot.callback = 0;
    slot.credits.store(0, std::memory_order_release);
    // The console does not return from unregistration while the client's
    // callback is still running, and titles rely on that: Banjo's audio
    // shutdown wakes its render thread once, and that thread only exits if the
    // callback has already cleared its "in callback" flag. The lock is released
    // while waiting (the callback's guest code submits frames), and the wait is
    // bounded in case the caller holds something the callback is blocked on.
    if (slot.callback_running && rex::system::XThread::GetCurrentThread() != worker_thread_.get()) {
      if (!callback_done_.wait_for(lock, std::chrono::seconds(1), [&] { return !slot.callback_running; })) {
        REXAPU_WARN("renut audio: callback of client {} still running after 1 s", index);
      }
    }
    std::unique_ptr<AAudioDriver> driver = std::move(slot.driver);
    const uint32_t wrapped = slot.wrapped_arg;
    slot.wrapped_arg = 0;
    lock.unlock();
    driver.reset();  // closes the stream; the title is not listening any more
    if (wrapped) memory()->SystemHeapFree(wrapped);
    lock.lock();
    slot.state = Slot::State::kFree;
    return 0;
  }

  uint32_t Submit(uint32_t handle, uint32_t samples_ptr) {
    // Whichever guest thread hands frames over is the title's mixer (in Banjo
    // its 5.1 DSP thread): as much a part of the audio path as the worker.
    thread_local bool raised = false;
    if (!raised) {
      raised = true;
      RaiseThread("title mixer", gettid());
    }
    const size_t index = handle & 0xFFFF;
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= kSlotCount || slots_[index].state != Slot::State::kLive) return 0;
    slots_[index].driver->SubmitFrame(samples_ptr);
    return 0;
  }

  // ---- from the device's thread ----

  // Called with the number of slots the callback finished: lock-free, because
  // it runs in a real-time thread.
  void Credit(size_t slot, uint32_t frames) {
    slots_[slot].credits.fetch_add(frames, std::memory_order_acq_rel);
    Wake();
  }
  void Wake() {
    wake_.fetch_add(1, std::memory_order_seq_cst);
    if (sleeping_.load(std::memory_order_seq_cst)) FutexWake(&wake_);
  }

 protected:
  // The SDK's own client path is not used: the XAudio exports come here.
  rex::X_STATUS CreateDriver(size_t, rex::thread::Semaphore*, rex::audio::AudioDriver**) override {
    return X_STATUS_UNSUCCESSFUL;
  }
  void DestroyDriver(rex::audio::AudioDriver* driver) override { delete driver; }

 private:
  // Worker only, with the lock held. A starved output earns the title four more
  // frames of lead (up to 40, 213 ms), at once; a second of calm per frame takes
  // them back, so a loading screen is buffered deep and the game itself is not.
  void AdaptDepth(Slot& slot) {
    constexpr uint32_t kStep = 4, kMaxDepth = 40;
    const auto now = std::chrono::steady_clock::now();
    if (slot.driver->TakeUnderruns()) {
      slot.last_starved = now;
      const uint32_t grown = std::min(slot.depth + kStep, kMaxDepth);
      slot.credits.fetch_add(grown - slot.depth, std::memory_order_acq_rel);
      slot.depth = grown;
      slot.debt = 0;
    } else if (slot.depth > slot.base_depth && now - slot.last_starved >= std::chrono::seconds(1)) {
      slot.last_starved = now;
      --slot.depth;
      ++slot.debt;
    }
    // A credit the title is no longer owed: take it back instead of issuing it.
    while (slot.debt && TakeCredit(slot)) --slot.debt;
  }

  bool TakeCredit(Slot& slot) {
    uint32_t credits = slot.credits.load(std::memory_order_acquire);
    while (credits) {
      if (slot.credits.compare_exchange_weak(credits, credits - 1, std::memory_order_acq_rel)) return true;
    }
    return false;
  }

  void WorkerMain() {
    renut::MarkWaitProbeThread();
    renut::SetSignalerCallback(&MixerFound);
    RaiseThread("Audio Worker", gettid());
    PinToFastCores("Audio Worker", gettid());
    bool xma_raised = RaiseThreadNamed("XMA Decoder");
    uint32_t tune_countdown = 0;
    auto last_report = std::chrono::steady_clock::now();
    uint64_t reported_silent = 0;
    uint32_t xma_retries = 0;
    uint64_t callbacks = 0;
    double callback_ms = 0.0, callback_max = 0.0, callback_cpu_ms = 0.0;
    int last_cpu = -1;
    while (worker_running_) {
      const uint32_t seen = wake_.load(std::memory_order_acquire);
      bool did_work = false;
      for (size_t i = 0; i < kSlotCount; ++i) {
        Slot& slot = slots_[i];
        uint32_t callback = 0, argument = 0;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (slot.state != Slot::State::kLive) continue;
          if (slot.driver->NeedsReopen()) slot.driver->Reopen();
          AdaptDepth(slot);
          if (!TakeCredit(slot)) continue;
          callback = slot.callback;
          argument = slot.wrapped_arg;
          slot.callback_running = true;
        }
        uint64_t args[] = {argument};
        const auto started = std::chrono::steady_clock::now();
        timespec cpu_before{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_before);
        function_dispatcher_->Execute(worker_thread_->thread_state(), callback, args, std::size(args));
        {
          const double ms =
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
          timespec cpu_after{};
          clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_after);
          callback_cpu_ms += double(cpu_after.tv_sec - cpu_before.tv_sec) * 1e3 +
                             double(cpu_after.tv_nsec - cpu_before.tv_nsec) / 1e6;
          last_cpu = sched_getcpu();
          ++callbacks;
          callback_ms += ms;
          callback_max = std::max(callback_max, ms);
        }
        {
          std::lock_guard<std::mutex> lock(mutex_);
          slot.callback_running = false;
        }
        callback_done_.notify_all();
        did_work = true;
      }
      if (!worker_running_) break;

      if (paused_) {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          for (Slot& slot : slots_) {
            if (slot.state == Slot::State::kLive) slot.driver->Pause();
          }
        }
        // Pause() set this to wake a worker blocked in the SDK's wait.
        rex::thread::Wait(shutdown_event_.get(), false, std::chrono::milliseconds(0));
        pause_fence_.Signal();
        rex::thread::Wait(resume_event_.get(), false);
        std::lock_guard<std::mutex> lock(mutex_);
        for (Slot& slot : slots_) {
          if (slot.state == Slot::State::kLive) slot.driver->Resume();
        }
        continue;
      }

      if (++tune_countdown >= 8) {
        tune_countdown = 0;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          for (Slot& slot : slots_) {
            if (slot.state == Slot::State::kLive) slot.driver->Tune();
          }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(5)) {
          last_report = now;
          const uint64_t total = g_silent_frames.load(std::memory_order_relaxed);
          // What the worker waited on in these seconds: always taken, so that
          // the window is these five seconds; printed only when they had silence,
          // which is when it is wanted.
          renut::WaitProbeEntry waits[4];
          size_t wait_count = 0;
          renut::TakeWaitProbe(waits, 4, &wait_count);
          if (total != reported_silent) {
            uint32_t depth = 0;
            for (Slot& slot : slots_) depth = std::max(depth, slot.depth);
            RENUT_AUDIO_LOG(INFO,
                            "%llu frames of silence in the last 5 s (%llu in all); %llu title callbacks, "
                            "mean %.2f ms (cpu %.2f ms), max %.1f ms, on cpu %d; queue %u frames",
                            static_cast<unsigned long long>(total - reported_silent),
                            static_cast<unsigned long long>(total), static_cast<unsigned long long>(callbacks),
                            callbacks ? callback_ms / double(callbacks) : 0.0,
                            callbacks ? callback_cpu_ms / double(callbacks) : 0.0, callback_max, last_cpu, depth);
            reported_silent = total;
            static const char* kKinds[] = {"?", "delay", "yield", "wait", "waitex", "waitmulti", "waitmultiex",
                                           "signalwait"};
            for (size_t i = 0; i < wait_count; ++i) {
              if (waits[i].what >= 20) {
                // A dispatcher header's type, noted when an address first showed up.
                RENUT_AUDIO_LOG(INFO, "  waits on %08X: kernel object type %u (slot %u)", waits[i].object,
                                waits[i].what - 20, waits[i].caller);
                continue;
              }
              RENUT_AUDIO_LOG(INFO, "  worker wait: %s (%u objects) from %08X: %llu times, %.2f ms each",
                              kKinds[waits[i].what < 8 ? waits[i].what : 0], waits[i].object, waits[i].caller,
                              static_cast<unsigned long long>(waits[i].count),
                              double(waits[i].nanoseconds) / double(waits[i].count) / 1e6);
            }
          }
          callbacks = 0;
          callback_ms = callback_max = callback_cpu_ms = 0.0;
        }
      }
      if (!xma_raised && ++xma_retries % 256 == 0 && xma_retries <= 1024) xma_raised = RaiseThreadNamed("XMA Decoder");

      if (did_work) continue;
      // Idle: sleep until a credit comes back. The timeout bounds how long
      // Pause() and Shutdown() wait, and rechecks a stream that went away.
      sleeping_.store(true, std::memory_order_seq_cst);
      FutexWait(&wake_, seen, std::chrono::milliseconds(20));
      sleeping_.store(false, std::memory_order_relaxed);
    }
    worker_running_ = false;
  }

  std::mutex mutex_;                       // slots; never held across the title's callback
  std::condition_variable callback_done_;  // a callback finished (Unregister waits on it)
  Slot slots_[kSlotCount];
  std::atomic<uint32_t> wake_{0};
  std::atomic<bool> sleeping_{false};
};

void AAudioDriver::ErrorCallback(AAudioStream*, void* user, aaudio_result_t error) {
  auto* self = static_cast<AAudioDriver*>(user);
  RENUT_AUDIO_LOG(WARN, "client %zu stream error: %s", self->slot_, AAudio_convertResultToText(error));
  self->disconnected_.store(true, std::memory_order_release);
  self->owner_->Wake();
}

void AAudioDriver::Render(float* out, int32_t frames) {
  // A gap in the stream is a click wherever the signal is not already zero, so
  // the edges of a gap are ramps: the output fades out from the last sample it
  // played, and fades back in when frames return.
  constexpr int32_t kRamp = 48;  // 1 ms
  int32_t done = 0;
  uint32_t finished = 0;
  uint32_t tail = tail_.load(std::memory_order_relaxed);
  const uint32_t head = head_.load(std::memory_order_acquire);
  while (done < frames && tail != head) {
    const float* slot = ring_.get() + (tail & (kRingFrames - 1)) * kFrameSamples * kOutChannels;
    const int32_t n = std::min<int32_t>(frames - done, kFrameSamples - read_pos_);
    std::memcpy(out + done * kOutChannels, slot + read_pos_ * kOutChannels, n * kOutChannels * sizeof(float));
    if (starved_ && done == 0) {
      const int32_t ramp = std::min(n, kRamp);
      for (int32_t i = 0; i < ramp; ++i) {
        const float gain = float(i + 1) / float(ramp + 1);
        out[i * 2] *= gain;
        out[i * 2 + 1] *= gain;
      }
      starved_ = false;
    }
    done += n;
    read_pos_ += n;
    if (read_pos_ == kFrameSamples) {
      read_pos_ = 0;
      ++tail;
      ++finished;
      tail_.store(tail, std::memory_order_release);  // the producer may reuse that slot now
    }
  }
  if (done > 0) {
    last_left_ = out[(done - 1) * 2];
    last_right_ = out[(done - 1) * 2 + 1];
  }
  if (done < frames) {
    int32_t at = done;
    if (!starved_) {
      starved_ = true;
      const int32_t fade = std::min(frames - at, kRamp);
      for (int32_t i = 0; i < fade; ++i) {
        const float gain = 1.0f - float(i + 1) / float(fade + 1);
        out[(at + i) * 2] = last_left_ * gain;
        out[(at + i) * 2 + 1] = last_right_ * gain;
      }
      at += fade;
      last_left_ = last_right_ = 0.0f;
      // Silence before the title has sent anything is just the start of a stream.
      if (got_frame_.load(std::memory_order_relaxed)) {
        underrun_events_.fetch_add(1, std::memory_order_relaxed);
      }
    }
    std::memset(out + at * kOutChannels, 0, (frames - at) * kOutChannels * sizeof(float));
    if (got_frame_.load(std::memory_order_relaxed)) {
      g_silent_frames.fetch_add((frames - done + kFrameSamples - 1) / kFrameSamples, std::memory_order_relaxed);
    }
  }
  if (finished) owner_->Credit(slot_, finished);
}

}  // namespace

std::unique_ptr<rex::system::IAudioSystem> Create(rex::runtime::FunctionDispatcher* dispatcher) {
  return std::make_unique<RenutAudioSystem>(dispatcher);
}

uint32_t RegisterRenderClient(uint32_t callback_ptr, uint32_t driver_ptr) {
  RenutAudioSystem* system = g_system.load(std::memory_order_acquire);
  return system ? system->Register(callback_ptr, driver_ptr) : static_cast<uint32_t>(X_STATUS_UNSUCCESSFUL);
}
uint32_t UnregisterRenderClient(uint32_t driver) {
  RenutAudioSystem* system = g_system.load(std::memory_order_acquire);
  return system ? system->Unregister(driver) : 0;
}
uint32_t SubmitRenderFrame(uint32_t driver, uint32_t samples_ptr) {
  RenutAudioSystem* system = g_system.load(std::memory_order_acquire);
  return system ? system->Submit(driver, samples_ptr) : 0;
}
uint64_t SilentFrames() { return g_silent_frames.load(std::memory_order_relaxed); }

}  // namespace renut::audio

extern "C" uint64_t renut_audio_silent_frames() { return renut::audio::SilentFrames(); }
