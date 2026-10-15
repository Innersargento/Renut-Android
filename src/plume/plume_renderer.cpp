#include "plume_renderer.h"

#include "frame_pacing.h"
#include "plume_vulkan.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(__ANDROID__)
// The session's own Vulkan loader (the bundled driver), opened by android_vulkan_loader.cpp.
void* renutXenosVulkanSymbol(const char* name);
void renutSetPlumeVulkanEntry(PFN_vkGetInstanceProcAddr entry);
#endif

REXCVAR_DEFINE_BOOL(renut_plume_vsync, true, "reNut/Plume",
                    "Present through the display's vertical sync (FIFO). Off asks for immediate "
                    "presentation where the driver has it, so frames are not held to the refresh rate");

namespace renut {
using namespace plume;

namespace {
// The platform window as Plume's swapchain wants it, and the reference the
// renderer keeps on it (the Android surface is reference counted; an HWND is not).
#if defined(__ANDROID__)
RenderWindow ToRenderWindow(PlumeWindow* window) { return window; }
void AcquireWindow(PlumeWindow* window) { ANativeWindow_acquire(window); }
void ReleaseWindow(PlumeWindow* window) { ANativeWindow_release(window); }
#else
RenderWindow ToRenderWindow(PlumeWindow* window) { return reinterpret_cast<RenderWindow>(window); }
void AcquireWindow(PlumeWindow*) {}
void ReleaseWindow(PlumeWindow*) {}
#endif
}  // namespace

namespace {
std::mutex instance_mutex;
std::unique_ptr<PlumeRenderer> instance;

void Require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
}  // namespace

struct PlumeRenderer::State {
  PlumeWindow* window = nullptr;
  std::string gpu;
  std::unique_ptr<VulkanInterface> api;
  std::unique_ptr<RenderDevice> device;
  std::unique_ptr<RenderCommandQueue> queue;
  std::unique_ptr<RenderSwapChain> swap;
  // One set per frame in flight. A slot's fence guards everything that frame
  // submitted, including the command list the processor recorded into it, so
  // reusing a slot means waiting on it first.
  struct Slot {
    std::unique_ptr<RenderCommandList> commands;
    std::unique_ptr<RenderCommandFence> fence;
    std::unique_ptr<RenderCommandSemaphore> acquire;
    bool pending = false;
  };
  std::array<Slot, kPlumeFramesInFlight> slots;
  std::vector<std::unique_ptr<RenderCommandSemaphore>> release;
  RenderFormat format = RenderFormat::UNKNOWN;
  // Background: the frame loop parks while `paused`. `swap_mutex` covers every use
  // of the swapchain, so AttachWindow can replace it between two presents.
  std::mutex swap_mutex;
  std::mutex pause_mutex;
  std::condition_variable pause_cv;
  bool paused = false;
  bool stopping = false;
  void WaitWhilePaused() {
    std::unique_lock lock(pause_mutex);
    pause_cv.wait(lock, [this] { return !paused || stopping; });
  }
  std::mutex color_mutex;
  std::array<float, 4> clear_color{0.f, 0.f, 0.f, 1.f};
  std::atomic<uint64_t> frames{0};

  ~State() {
    if (device) {
      vkDeviceWaitIdle(static_cast<VulkanDevice*>(device.get())->vk);
    }
    release.clear();
    for (auto& slot : slots) {
      slot.acquire.reset();
      slot.fence.reset();
      slot.commands.reset();
    }
    swap.reset();
    queue.reset();
    device.reset();
    api.reset();
    if (window) ReleaseWindow(window);
  }
};

namespace {

// The pipeline cache kept on disk between runs. Plume passes no cache, which
// makes the driver recompile every pipeline on every launch: the first pass
// through Showdown Town spent 67% of its frame time inside
// vkCreateGraphicsPipelines, and none of that work survived.
//
// Every pipeline Plume creates comes through the Vulkan layer at the end of this
// file (it stands in for the two vkCreate*Pipelines entry points). When the driver can be asked for a
// pipeline without compiling one (VK_EXT_pipeline_creation_cache_control), it
// is looked up in `shared` first, which holds everything compiled so far; one
// that is not there is compiled into an empty cache of its own and then merged
// into `shared` and into `delta`. So no lookup ever waits behind a compile, and
// what is written while the game runs is `delta` - what is new - instead of the
// whole blob: that grows past 300 MB, and copying it out held the driver's
// cache lock, and so every pipeline creation, for as long as the copy took.
// The delta is folded into the base file at launch once it is large.
//
// A driver without the extension gets the old arrangement: one cache, written
// whole.
struct PipelineCache {
  std::mutex mutex;  // loading; `path` and `delta` are set under it
  std::string path;
  std::atomic<VkDevice> device{VK_NULL_HANDLE};
  bool lookup = false;  // the driver honours FAIL_ON_PIPELINE_COMPILE_REQUIRED
  std::atomic<VkPipelineCache> shared{VK_NULL_HANDLE};
  VkPipelineCache delta = VK_NULL_HANDLE;
  // vkMergePipelineCaches wants its destination externally synchronized:
  // merges take these exclusively, lookups in `shared` take it shared.
  std::shared_mutex shared_merge;
  std::mutex delta_merge;
  std::atomic<bool> dirty{false};
  std::atomic<bool> saving{false};
  std::chrono::steady_clock::time_point saved{};
  std::atomic<uint64_t> found{0}, compiled{0};
};
PipelineCache pipeline_cache;
thread_local bool pipeline_lookup_only = false;

// What the Vulkan layer (end of the file) keeps: the entry points it replaced,
// and what it found out about the device.
struct VulkanLayerState {
  PFN_vkCreateDevice create_device = nullptr;
  PFN_vkCreateGraphicsPipelines create_graphics = nullptr;
  PFN_vkCreateComputePipelines create_compute = nullptr;
  PFN_vkCreateBuffer create_buffer = nullptr;
  PFN_vkQueueSubmit queue_submit = nullptr;
  PFN_vkWaitForFences wait_for_fences = nullptr;
  bool installed = false;
  bool cache_control = false;               // VK_EXT_pipeline_creation_cache_control is on
  bool conditional = false;                 // VK_EXT_conditional_rendering is on (Hi-Z)
  std::atomic<bool> buffer_address{false};  // buffers get the device address usage
  std::atomic<bool> device_lost{false};
};
VulkanLayerState layer;

// plume-pipelines.bin -> plume-pipelines.delta.bin
std::string DeltaPath(const std::string& path) {
  const std::string suffix = ".bin";
  if (path.size() > suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) {
    return path.substr(0, path.size() - suffix.size()) + ".delta.bin";
  }
  return path + ".delta";
}

VkPipelineCache CreateEmptyCache(VkDevice device) {
  VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  VkPipelineCache cache = VK_NULL_HANDLE;
  if (vkCreatePipelineCache(device, &info, nullptr, &cache) != VK_SUCCESS) return VK_NULL_HANDLE;
  return cache;
}

// A cache seeded from `path`, read through a mapping rather than copied into
// memory first. A blob the driver does not recognise (another device or driver
// version) is ignored or refused, and the cache starts empty.
VkPipelineCache LoadCacheFile(VkDevice device, const std::string& path, size_t* bytes) {
  *bytes = 0;
  void* data = nullptr;
  size_t size = 0;
#if defined(_WIN32)
  // No mapping here: the file is read into memory.
  std::vector<uint8_t> contents;
  if (!path.empty()) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (file) {
      const auto length = file.tellg();
      if (length > 0) {
        contents.resize(size_t(length));
        file.seekg(0);
        if (file.read(reinterpret_cast<char*>(contents.data()), std::streamsize(length))) {
          data = contents.data();
          size = contents.size();
        }
      }
    }
  }
#else
  const int fd = path.empty() ? -1 : open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    struct stat st {};
    if (fstat(fd, &st) == 0 && st.st_size > 0) {
      void* mapped = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
      if (mapped != MAP_FAILED) {
        madvise(mapped, size_t(st.st_size), MADV_SEQUENTIAL);
        data = mapped;
        size = size_t(st.st_size);
      }
    }
    close(fd);
  }
#endif
  VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  info.initialDataSize = size;
  info.pInitialData = data;
  VkPipelineCache cache = VK_NULL_HANDLE;
  if (vkCreatePipelineCache(device, &info, nullptr, &cache) == VK_SUCCESS) {
    *bytes = size;
  } else {
    cache = CreateEmptyCache(device);
  }
#if !defined(_WIN32)
  if (data) munmap(data, size);
#endif
  return cache;
}

// Writes `cache` beside `path` and renames it over: Android kills the process
// without warning, and a truncated blob is one the driver rejects on the next
// run. The copy goes straight into a mapping of the file, so even the whole
// blob costs the process no memory of its own; the space is reserved first, so
// a full disk fails the write instead of faulting inside the copy.
bool WriteCacheFile(VkDevice device, VkPipelineCache cache, const std::string& path, size_t* written) {
  size_t size = 0;
  if (vkGetPipelineCacheData(device, cache, &size, nullptr) != VK_SUCCESS || !size) return false;
  // Room for what other threads add between the two calls.
  const size_t room = size + size / 16 + (size_t(1) << 20);
  const std::string temporary = path + ".tmp";
#if defined(_WIN32)
  std::vector<uint8_t> data(room);
  size = room;
  if (vkGetPipelineCacheData(device, cache, &size, data.data()) != VK_SUCCESS) return false;
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(size))) return false;
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    return false;
  }
  if (written) *written = size;
  return true;
#else
  const int fd = open(temporary.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  bool ok = false;
  if (posix_fallocate(fd, 0, off_t(room)) == 0) {
    void* map = mmap(nullptr, room, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map != MAP_FAILED) {
      size = room;
      ok = vkGetPipelineCacheData(device, cache, &size, map) == VK_SUCCESS;
      munmap(map, room);
    }
  } else {
    // A file system without fallocate: through memory, then.
    std::vector<uint8_t> data(room);
    size = room;
    ok = vkGetPipelineCacheData(device, cache, &size, data.data()) == VK_SUCCESS &&
         write(fd, data.data(), size) == ssize_t(size);
  }
  ok = ok && ftruncate(fd, off_t(size)) == 0;
  close(fd);
  ok = ok && std::rename(temporary.c_str(), path.c_str()) == 0;
  if (!ok) {
    std::remove(temporary.c_str());
  } else if (written) {
    *written = size;
  }
  return ok;
#endif
}

// Called with `mutex` held, before anything else can use the caches.
VkPipelineCache LoadPipelineCaches(VkDevice device) {
  PipelineCache& c = pipeline_cache;
  const auto start = std::chrono::steady_clock::now();
  size_t base_bytes = 0, delta_bytes = 0, folded = 0;
  VkPipelineCache shared = LoadCacheFile(device, c.path, &base_bytes);
  if (shared == VK_NULL_HANDLE) return VK_NULL_HANDLE;
  if (c.lookup) {
    const std::string delta_path = DeltaPath(c.path);
    VkPipelineCache delta = LoadCacheFile(device, delta_path, &delta_bytes);
    if (delta != VK_NULL_HANDLE) vkMergePipelineCaches(device, shared, 1, &delta);
    // Once the delta is a sizeable part of the whole, it goes into the base file:
    // written here, before `shared` is handed out, so no lookup waits on the copy.
    // Capped, so the delta written while the game runs stays small.
    const size_t fold_at = std::max(size_t(8) << 20, std::min(size_t(32) << 20, base_bytes / 4));
    if (delta_bytes > fold_at && !c.path.empty() &&
        WriteCacheFile(device, shared, c.path, &folded)) {
      std::remove(delta_path.c_str());
      if (delta != VK_NULL_HANDLE) vkDestroyPipelineCache(device, delta, nullptr);
      delta = VK_NULL_HANDLE;
    }
    c.delta = delta != VK_NULL_HANDLE ? delta : CreateEmptyCache(device);
    // Nowhere to keep what is new: back to one cache, written whole.
    if (c.delta == VK_NULL_HANDLE) c.lookup = false;
  }
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  REXLOG_INFO("Renut: Plume pipeline cache {} ({} KB from disk, {} KB of it new since the last fold; {}{} ms; {})",
              base_bytes || delta_bytes ? "carregado" : "criado", (base_bytes + delta_bytes) >> 10,
              delta_bytes >> 10, folded ? fmt::format("folded into {} KB, ", folded >> 10) : std::string(), ms,
              c.lookup ? "lookups without compiling" : "one cache, written whole");
  c.device.store(device);
  c.shared.store(shared, std::memory_order_release);
  return shared;
}

VkPipelineCache SharedPipelineCache(VkDevice device) {
  PipelineCache& c = pipeline_cache;
  VkPipelineCache shared = c.shared.load(std::memory_order_acquire);
  if (shared != VK_NULL_HANDLE && c.device.load() == device) return shared;
  std::lock_guard lock(c.mutex);
  shared = c.shared.load(std::memory_order_acquire);
  if (shared != VK_NULL_HANDLE && c.device.load() == device) return shared;
  return LoadPipelineCaches(device);
}

// Loading a large cache takes a while, so it starts as soon as both the device
// and the file are known, off the thread that boots the game. A pipeline asked
// for meanwhile waits for it, as it would have anyway.
void PreloadPipelineCache(VkDevice device) {
  if (device == VK_NULL_HANDLE) return;
  {
    std::lock_guard lock(pipeline_cache.mutex);
    if (pipeline_cache.path.empty() || pipeline_cache.shared.load() != VK_NULL_HANDLE) return;
  }
  std::thread([device] { SharedPipelineCache(device); }).detach();
}

void WritePlumePipelineCache() {
  PipelineCache& c = pipeline_cache;
  const VkPipelineCache shared = c.shared.load(std::memory_order_acquire);
  const VkDevice device = c.device.load();
  std::string path;
  {
    std::lock_guard lock(c.mutex);
    path = c.path;
  }
  if (shared == VK_NULL_HANDLE || path.empty()) return;
  c.dirty = false;
  bool ok = false;
  if (c.lookup) {
    // A snapshot of the delta, so a merge into it waits for the snapshot and
    // not for the copy.
    VkPipelineCache snapshot = CreateEmptyCache(device);
    if (snapshot != VK_NULL_HANDLE) {
      {
        std::lock_guard lock(c.delta_merge);
        vkMergePipelineCaches(device, snapshot, 1, &c.delta);
      }
      ok = WriteCacheFile(device, snapshot, DeltaPath(path), nullptr);
      vkDestroyPipelineCache(device, snapshot, nullptr);
    }
  } else {
    ok = WriteCacheFile(device, shared, path, nullptr);
  }
  if (!ok) c.dirty = true;
}
}  // namespace

void SetPlumePipelineCachePath(const std::string& path) {
  {
    std::lock_guard lock(pipeline_cache.mutex);
    pipeline_cache.path = path;
  }
  if (PlumeRenderer* renderer = PlumeRenderer::Get()) {
    PreloadPipelineCache(static_cast<VulkanDevice*>(renderer->device())->vk);
  }
}

// On a thread of its own. What is written is only what is new (see
// PipelineCache), so it can go out every few seconds; a driver without lookups
// still writes the whole blob, at most once a minute. `now` skips the wait, for
// when the app leaves the screen and may be killed.
void SavePlumePipelineCache(bool now) {
  PipelineCache& c = pipeline_cache;
  if (!c.dirty.load()) return;
  bool idle = false;
  if (!c.saving.compare_exchange_strong(idle, true)) return;
  const auto time = std::chrono::steady_clock::now();
  const auto interval = c.lookup ? std::chrono::seconds(15) : std::chrono::seconds(60);
  if (!now && c.saved.time_since_epoch().count() && time - c.saved < interval) {
    c.saving = false;
    return;
  }
  c.saved = time;
  std::thread([] {
    WritePlumePipelineCache();
    pipeline_cache.saving = false;
  }).detach();
}

bool PlumePipelineLookupSupported() { return pipeline_cache.lookup; }

PlumePipelineLookupOnly::PlumePipelineLookupOnly() { pipeline_lookup_only = true; }
PlumePipelineLookupOnly::~PlumePipelineLookupOnly() { pipeline_lookup_only = false; }

void PlumePipelineCacheCounts(uint64_t* found, uint64_t* compiled) {
  *found = pipeline_cache.found.load(std::memory_order_relaxed);
  *compiled = pipeline_cache.compiled.load(std::memory_order_relaxed);
}

void InstallVulkanLayer();
void VulkanLayerDeviceCreated(bool buffer_device_address);

PlumeRenderer::PlumeRenderer() : state_(std::make_unique<State>()) {}
PlumeRenderer::~PlumeRenderer() = default;

bool PlumeRenderer::Start(PlumeWindow* window, std::string* error) {
  std::lock_guard lock(instance_mutex);
  if (instance) return true;
  try {
#if defined(__ANDROID__)
    auto entry = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        renutXenosVulkanSymbol("vkGetInstanceProcAddr"));
    Require(entry, "Plume: carregador Vulkan da sessão indisponível.");
    renutSetPlumeVulkanEntry(entry);
#endif
    std::unique_ptr<PlumeRenderer> renderer(new PlumeRenderer());
    auto& s = *renderer->state_;
    s.window = window;
    AcquireWindow(window);
    s.api = std::make_unique<VulkanInterface>();
    Require(s.api->isValid(), "Plume: instância Vulkan inválida.");
    // The instance has loaded the entry points; the layer replaces the few it
    // needs to see before the device is made.
    InstallVulkanLayer();
    s.device = s.api->createDevice("");
    Require(bool(s.device), "Plume: dispositivo indisponível.");
    VulkanLayerDeviceCreated(s.device->getCapabilities().bufferDeviceAddress);
    s.gpu = s.device->getDescription().name;
    s.queue = s.device->createCommandQueue(RenderCommandListType::DIRECT);
    Require(bool(s.queue), "Plume: fila indisponível.");
    // The Android surface reports RGBA8 on the tested devices; fall back to BGRA8.
    auto* device = static_cast<VulkanDevice*>(s.device.get());
    VkSurfaceKHR surface = VK_NULL_HANDLE;
#if defined(__ANDROID__)
    VkAndroidSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    info.window = window;
    Require(vkCreateAndroidSurfaceKHR(s.api->instance, &info, nullptr, &surface) == VK_SUCCESS,
            "Plume: superfície indisponível.");
#else
    VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    info.hinstance = GetModuleHandleW(nullptr);
    info.hwnd = reinterpret_cast<HWND>(window);
    Require(vkCreateWin32SurfaceKHR(s.api->instance, &info, nullptr, &surface) == VK_SUCCESS,
            "Plume: superfície indisponível.");
#endif
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device->physicalDevice, surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(device->physicalDevice, surface, &count, formats.data());
    vkDestroySurfaceKHR(s.api->instance, surface, nullptr);
    for (auto f : formats) {
      if (f.format == VK_FORMAT_R8G8B8A8_UNORM) {
        s.format = RenderFormat::R8G8B8A8_UNORM;
        break;
      }
      if (f.format == VK_FORMAT_B8G8R8A8_UNORM) s.format = RenderFormat::B8G8R8A8_UNORM;
    }
    Require(s.format != RenderFormat::UNKNOWN, "Plume: superfície sem RGBA8/BGRA8.");
    s.swap = s.queue->createSwapChain(RenderSwapChainDesc(ToRenderWindow(window), s.format, 3));
    if (s.swap) s.swap->setVsyncEnabled(REXCVAR_GET(renut_plume_vsync));
    Require(s.swap && s.swap->resize() && s.swap->getTextureCount(), "Plume: swapchain inválida.");
    for (auto& slot : s.slots) {
      slot.commands = s.queue->createCommandList();
      slot.fence = s.device->createCommandFence();
      slot.acquire = s.device->createCommandSemaphore();
      Require(slot.commands && slot.fence && slot.acquire,
              "Plume: falha ao alocar a apresentação.");
    }
    for (uint32_t i = 0; i < s.swap->getTextureCount(); ++i) {
      s.release.push_back(s.device->createCommandSemaphore());
    }
    REXLOG_INFO("Renut: Plume renderer started on {} ({}x{})", s.gpu, s.swap->getWidth(),
                s.swap->getHeight());
    {
      std::lock_guard cache_lock(pipeline_cache.mutex);
      pipeline_cache.lookup = layer.cache_control;
    }
    PreloadPipelineCache(device->vk);
    instance = std::move(renderer);
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    REXLOG_ERROR("Renut: {}", e.what());
    return false;
  }
}

void PlumeRenderer::Stop() {
  std::lock_guard lock(instance_mutex);
  instance.reset();
}

PlumeRenderer* PlumeRenderer::Get() { return instance.get(); }

void PlumeRenderer::SetClearColor(float r, float g, float b, float a) {
  std::lock_guard lock(state_->color_mutex);
  state_->clear_color = {r, g, b, a};
}

void PlumeRenderer::Present() {
  auto& s = *state_;
  std::lock_guard swap_lock(s.swap_mutex);
  if (!s.swap) return;
  std::array<float, 4> color;
  {
    std::lock_guard lock(s.color_mutex);
    color = s.clear_color;
  }
  // Before the game draws anything there is no pipelining to preserve, so this
  // path keeps slot 0 and waits.
  WaitForAllSlots();
  auto& slot = s.slots[0];
  uint32_t index = 0;
  if (!s.swap->acquireTexture(slot.acquire.get(), &index)) {
    s.swap->resize();
    return;
  }
  auto* target = s.swap->getTexture(index);
  slot.commands->begin();
  slot.commands->barriers(RenderBarrierStage::COPY,
                          RenderTextureBarrier(target, RenderTextureLayout::COPY_DEST));
  VkClearColorValue value{};
  for (int i = 0; i < 4; ++i) value.float32[i] = color[i];
  VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdClearColorImage(static_cast<VulkanCommandList*>(slot.commands.get())->vk,
                       static_cast<VulkanTexture*>(target)->vk,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
  slot.commands->barriers(RenderBarrierStage::NONE,
                          RenderTextureBarrier(target, RenderTextureLayout::PRESENT));
  slot.commands->end();
  const RenderCommandList* list = slot.commands.get();
  auto* wait = slot.acquire.get();
  auto* signal = s.release[index].get();
  s.queue->executeCommandLists(&list, 1, &wait, 1, &signal, 1, slot.fence.get());
  s.swap->present(index, &signal, 1);
  s.queue->waitForCommandFence(slot.fence.get());
  if (s.frames.fetch_add(1) == 0) {
    REXLOG_INFO("Renut: Plume presented its first frame");
  }
}

RenderDevice* PlumeRenderer::device() const { return state_->device.get(); }
RenderCommandQueue* PlumeRenderer::queue() const { return state_->queue.get(); }

void PlumeRenderer::WaitForSlot(uint32_t slot) {
  auto& s = *state_;
  auto& entry = s.slots[slot % kPlumeFramesInFlight];
  if (!entry.pending) return;
  s.queue->waitForCommandFence(entry.fence.get());
  entry.pending = false;
}

void PlumeRenderer::WaitForAllSlots() {
  for (uint32_t i = 0; i < kPlumeFramesInFlight; ++i) WaitForSlot(i);
}

void PlumeRenderer::SubmitAndPresent(RenderCommandList* frame, RenderTexture* source,
                                     uint32_t width, uint32_t height, uint32_t slot_index) {
  auto& s = *state_;
  // In the background the loop stops here, holding the finished frame.
  s.WaitWhilePaused();
  std::lock_guard swap_lock(s.swap_mutex);
  if (s.swap) {
    // How the swapchain presents follows the frame-rate target, live. On the
    // display's vertical sync (FIFO) every present waits for a refresh, so the
    // refresh rate (120 Hz on the fast panels) was a ceiling no matter what the
    // game or the pacer asked for. Above the sync ceiling, or unlimited, it
    // queues nothing: Mailbox replaces a waiting frame with the newer one, and
    // Immediate shows it at once where the driver has that and not Mailbox.
    auto* chain = static_cast<VulkanSwapChain*>(s.swap.get());
    VkPresentModeKHR want = VK_PRESENT_MODE_FIFO_KHR;
    if (!(pacing::PresentVsync() && REXCVAR_GET(renut_plume_vsync))) {
      if (chain->mailboxPresentModeSupported) {
        want = VK_PRESENT_MODE_MAILBOX_KHR;
      } else if (chain->immediatePresentModeSupported) {
        want = VK_PRESENT_MODE_IMMEDIATE_KHR;
      }
    }
    static VkPresentModeKHR refused = VK_PRESENT_MODE_MAX_ENUM_KHR;
    if (chain->createdPresentMode != want && want != refused) {
      // The images in flight go before the swapchain is replaced under them.
      WaitForAllSlots();
      chain->requiredPresentMode = want;
      if (chain->resize()) {
        while (s.release.size() < s.swap->getTextureCount()) {
          s.release.push_back(s.device->createCommandSemaphore());
        }
        REXLOG_INFO("Renut: Plume presents {} (target {} FPS)",
                    want == VK_PRESENT_MODE_FIFO_KHR      ? "on the vertical sync"
                    : want == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox"
                                                          : "immediate",
                    pacing::Target());
      } else {
        chain->requiredPresentMode = chain->createdPresentMode;
        refused = want;
        REXLOG_WARN("Renut: Plume could not change the present mode; keeping the current one");
      }
    }
  }
  auto& slot = s.slots[slot_index % kPlumeFramesInFlight];
  // The caller waits before it reuses a slot, so this one is free by now.
  WaitForSlot(slot_index);
  uint32_t index = 0;
  bool acquired = s.swap && s.swap->acquireTexture(slot.acquire.get(), &index);
  if (!acquired) {
    // Still run the frame: the guest's fences and resolved textures depend on it.
    if (frame) {
      const RenderCommandList* list = frame;
      s.queue->executeCommandLists(&list, 1, nullptr, 0, nullptr, 0, slot.fence.get());
      slot.pending = true;
    }
    if (s.swap) s.swap->resize();
    return;
  }
  auto* target = s.swap->getTexture(index);
  slot.commands->begin();
  slot.commands->barriers(RenderBarrierStage::COPY,
                          RenderTextureBarrier(target, RenderTextureLayout::COPY_DEST));
  VkCommandBuffer cb = static_cast<VulkanCommandList*>(slot.commands.get())->vk;
  VkImage dst = static_cast<VulkanTexture*>(target)->vk;
  VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  // Black around a letterboxed frame; the game's last clear colour otherwise.
  VkClearColorValue background{};
  if (!source) {
    std::lock_guard lock(s.color_mutex);
    for (int i = 0; i < 4; ++i) background.float32[i] = s.clear_color[i];
  }
  vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &background, 1, &range);
  if (source && width && height) {
    // Letterbox: keep the guest's aspect ratio inside the swapchain.
    const float sw = float(s.swap->getWidth()), sh = float(s.swap->getHeight());
    const float scale = std::min(sw / float(width), sh / float(height));
    const int32_t w = int32_t(float(width) * scale), h = int32_t(float(height) * scale);
    const int32_t x = (int32_t(sw) - w) / 2, y = (int32_t(sh) - h) / 2;
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(width), int32_t(height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {x, y, 0};
    blit.dstOffsets[1] = {x + w, y + h, 1};
    vkCmdBlitImage(cb, static_cast<VulkanTexture*>(source)->vk,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_LINEAR);
  }
  slot.commands->barriers(RenderBarrierStage::NONE,
                          RenderTextureBarrier(target, RenderTextureLayout::PRESENT));
  slot.commands->end();
  const RenderCommandList* lists[] = {frame, slot.commands.get()};
  auto* wait = slot.acquire.get();
  auto* signal = s.release[index].get();
  s.queue->executeCommandLists(frame ? lists : lists + 1, frame ? 2 : 1, &wait, 1, &signal, 1,
                               slot.fence.get());
  s.swap->present(index, &signal, 1);
  // No wait: the caller records the next frame while this one runs.
  slot.pending = true;
  if (s.frames.fetch_add(1) == 0) {
    REXLOG_INFO("Renut: Plume presented its first frame");
  }
}

void PlumeRenderer::PauseForBackground() {
  {
    std::lock_guard lock(state_->pause_mutex);
    state_->paused = true;
  }
  // Leaving the screen is when Android is most likely to kill the process.
  SavePlumePipelineCache(true);
}

void PlumeRenderer::SetPaused(bool paused) {
  {
    std::lock_guard lock(state_->pause_mutex);
    state_->paused = paused;
  }
  if (!paused) state_->pause_cv.notify_all();
}

void PlumeRenderer::ReleaseForShutdown() {
  {
    std::lock_guard lock(state_->pause_mutex);
    state_->stopping = true;
  }
  state_->pause_cv.notify_all();
}

bool PlumeRenderer::AttachWindow(PlumeWindow* window) {
  auto& s = *state_;
  if (!window) return false;
  // Waits for a present that was already under way when the surface died.
  std::lock_guard swap_lock(s.swap_mutex);
  try {
    AcquireWindow(window);
    // Everything queued before the pause has to finish before the old swapchain
    // goes: it may still hold images the presentation engine is showing.
    WaitForAllSlots();
    vkDeviceWaitIdle(static_cast<VulkanDevice*>(s.device.get())->vk);
    s.release.clear();
    s.swap.reset();
    if (s.window) ReleaseWindow(s.window);
    s.window = window;
    s.swap = s.queue->createSwapChain(RenderSwapChainDesc(ToRenderWindow(window), s.format, 3));
    if (s.swap) s.swap->setVsyncEnabled(REXCVAR_GET(renut_plume_vsync));
    Require(s.swap && s.swap->resize() && s.swap->getTextureCount(), "Plume: swapchain inválida.");
    for (uint32_t i = 0; i < s.swap->getTextureCount(); ++i) {
      s.release.push_back(s.device->createCommandSemaphore());
    }
    REXLOG_INFO("Renut: Plume swapchain recreated ({}x{})", s.swap->getWidth(), s.swap->getHeight());
  } catch (const std::exception& e) {
    REXLOG_ERROR("Renut: {}", e.what());
    return false;
  }
  {
    std::lock_guard lock(s.pause_mutex);
    s.paused = false;
  }
  s.pause_cv.notify_all();
  return true;
}

std::string PlumeRenderer::report() const {
  std::ostringstream out;
  out << "Plume: " << state_->gpu << ", quadros apresentados: " << state_->frames.load();
  return out.str();
}

}  // namespace renut

namespace {
// Looks the pipeline up first and compiles it only if the cache misses it, into
// a cache of its own that is merged afterwards (see PipelineCache).
template <typename Info, typename Create>
VkResult CreatePipelines(VkDevice device, uint32_t count, const Info* infos, const VkAllocationCallbacks* allocator,
                         VkPipeline* pipelines, Create create) {
  using namespace renut;
  PipelineCache& c = pipeline_cache;
  const VkPipelineCache shared = SharedPipelineCache(device);
  if (!c.lookup || shared == VK_NULL_HANDLE || count != 1) {
    if (pipeline_lookup_only) {
      for (uint32_t i = 0; i < count; ++i) pipelines[i] = VK_NULL_HANDLE;
      return VK_PIPELINE_COMPILE_REQUIRED;
    }
    const VkResult result = create(device, shared, count, infos, allocator, pipelines);
    if (result == VK_SUCCESS) c.dirty = true;
    return result;
  }
  Info info = *infos;
  info.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
  VkResult result;
  {
    std::shared_lock lock(c.shared_merge);
    result = create(device, shared, 1, &info, allocator, pipelines);
  }
  if (result != VK_PIPELINE_COMPILE_REQUIRED) {
    if (result == VK_SUCCESS) c.found.fetch_add(1, std::memory_order_relaxed);
    return result;
  }
  if (pipeline_lookup_only) return result;
  info.flags = infos->flags;
  VkPipelineCache own = CreateEmptyCache(device);
  result = create(device, own != VK_NULL_HANDLE ? own : shared, 1, &info, allocator, pipelines);
  if (own != VK_NULL_HANDLE) {
    if (result == VK_SUCCESS) {
      {
        std::unique_lock lock(c.shared_merge);
        vkMergePipelineCaches(device, shared, 1, &own);
      }
      {
        std::lock_guard lock(c.delta_merge);
        vkMergePipelineCaches(device, c.delta, 1, &own);
      }
      c.dirty = true;
    }
    vkDestroyPipelineCache(device, own, nullptr);
  }
  if (result == VK_SUCCESS) c.compiled.fetch_add(1, std::memory_order_relaxed);
  return result;
}
}  // namespace

// ---------------------------------------------------------------------------
// The Vulkan layer.
//
// Plume is used exactly as released. What the renderer needs from Vulkan and
// Plume's interface does not carry is done where Plume meets the driver, the
// way a Vulkan layer would: Plume calls the entry points through volk's global
// function pointers, so the few below are replaced once the instance has loaded
// them (InstallVulkanLayer), before the device is made.
//
//  - vkCreateDevice: enables VK_EXT_pipeline_creation_cache_control when the
//    driver has it, which is what lets a pipeline be asked for without being
//    compiled (the pipeline cache above).
//  - vkCreateGraphicsPipelines / vkCreateComputePipelines: the on-disk cache;
//    and, for graphics pipelines, what Plume's description cannot say: the
//    stencil reference and masks and the blend constants are dynamic state
//    (the Xbox sets them per draw), and the 8 and 16-bit integer vertex
//    attributes the shaders read as numbers are converted to float by the
//    hardware (the *_USCALED/_SSCALED formats Plume has no name for).
//  - vkCreateBuffer: storage buffers can also feed indirect draws and be
//    addressed by the shaders.
//  - vkQueueSubmit / vkWaitForFences: remember that the device was lost, which
//    every later call answers the same way.
// ---------------------------------------------------------------------------
namespace renut {
namespace {

// The vertex formats the renderer asks Plume for, by the names Plume has, and
// the Vulkan format each one stands for. UBYTE4/SHORT2/SHORT4 and the packed
// 2_10_10_10 arrive in the vertex shader as converted floats.
VkFormat LayerVertexFormat(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8G8B8A8_UINT: return VK_FORMAT_R8G8B8A8_USCALED;
    case VK_FORMAT_R8G8B8A8_SINT: return VK_FORMAT_R8G8B8A8_SSCALED;
    case VK_FORMAT_R16G16_UINT: return VK_FORMAT_R16G16_USCALED;
    case VK_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SSCALED;
    case VK_FORMAT_R16G16B16A16_UINT: return VK_FORMAT_R16G16B16A16_USCALED;
    case VK_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SSCALED;
    // Used as vertex attributes by nothing else: R8G8_UINT stands for the
    // unsigned normalized 2_10_10_10, R8G8_SINT for the unsigned scaled one.
    case VK_FORMAT_R8G8_UINT: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case VK_FORMAT_R8G8_SINT: return VK_FORMAT_A2B10G10R10_USCALED_PACK32;
    default: return format;
  }
}

bool UsesBlendConstant(const VkPipelineColorBlendAttachmentState& attachment) {
  auto constant = [](VkBlendFactor f) {
    return f == VK_BLEND_FACTOR_CONSTANT_COLOR || f == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR ||
           f == VK_BLEND_FACTOR_CONSTANT_ALPHA || f == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
  };
  return attachment.blendEnable &&
         (constant(attachment.srcColorBlendFactor) || constant(attachment.dstColorBlendFactor) ||
          constant(attachment.srcAlphaBlendFactor) || constant(attachment.dstAlphaBlendFactor));
}

VKAPI_ATTR VkResult VKAPI_CALL LayerCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                                 const VkAllocationCallbacks* allocator, VkDevice* device) {
  VkDeviceCreateInfo edited = *info;
  std::vector<const char*> extensions(info->ppEnabledExtensionNames,
                                      info->ppEnabledExtensionNames + info->enabledExtensionCount);
  VkPhysicalDevicePipelineCreationCacheControlFeatures control{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES};
  VkPhysicalDeviceConditionalRenderingFeaturesEXT conditional{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT};
  uint32_t count = 0;
  vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> available(count);
  vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, available.data());
  const auto named = [](const char* a, const char* b) { return std::strcmp(a, b) == 0; };
  const auto has = [&](const char* name) {
    return std::any_of(available.begin(), available.end(),
                       [&](const VkExtensionProperties& e) { return named(e.extensionName, name); });
  };
  const auto requested = [&](const char* name) {
    return std::any_of(extensions.begin(), extensions.end(), [&](const char* e) { return named(e, name); });
  };
  void* chain = const_cast<void*>(info->pNext);
  bool control_wanted = false, conditional_wanted = false;
  if (has(VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME) &&
      !requested(VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME)) {
    VkPhysicalDevicePipelineCreationCacheControlFeatures supported{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &supported;
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (supported.pipelineCreationCacheControl) {
      control.pipelineCreationCacheControl = VK_TRUE;
      control.pNext = chain;
      chain = &control;
      extensions.push_back(VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME);
      control_wanted = true;
    }
  }
  // Conditional rendering: the Hi-Z test's answer decides, on the GPU, whether
  // a draw runs.
  if (has(VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME) && !requested(VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME)) {
    VkPhysicalDeviceConditionalRenderingFeaturesEXT supported{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &supported;
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (supported.conditionalRendering) {
      conditional.conditionalRendering = VK_TRUE;
      conditional.pNext = chain;
      chain = &conditional;
      extensions.push_back(VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME);
      conditional_wanted = true;
    }
  }
  if (control_wanted || conditional_wanted) {
    edited.pNext = chain;
    edited.enabledExtensionCount = uint32_t(extensions.size());
    edited.ppEnabledExtensionNames = extensions.data();
  }
  const VkResult result = layer.create_device(physical, &edited, allocator, device);
  layer.cache_control = result == VK_SUCCESS && control_wanted;
  layer.conditional = result == VK_SUCCESS && conditional_wanted;
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL LayerCreateGraphicsPipelines(VkDevice device, VkPipelineCache, uint32_t count,
                                                            const VkGraphicsPipelineCreateInfo* infos,
                                                            const VkAllocationCallbacks* allocator,
                                                            VkPipeline* pipelines) {
  // Copies of the descriptions that change; what they point to is kept here
  // until the pipelines exist.
  struct Edited {
    VkPipelineDynamicStateCreateInfo dynamic;
    std::vector<VkDynamicState> states;
    VkPipelineVertexInputStateCreateInfo vertex;
    std::vector<VkVertexInputAttributeDescription> attributes;
  };
  thread_local std::vector<Edited> edited;
  thread_local std::vector<VkGraphicsPipelineCreateInfo> list;
  edited.assign(count, Edited{});
  list.assign(infos, infos + count);
  for (uint32_t i = 0; i < count; ++i) {
    Edited& e = edited[i];
    VkGraphicsPipelineCreateInfo& info = list[i];
    if (info.pDynamicState) {
      e.states.assign(info.pDynamicState->pDynamicStates,
                      info.pDynamicState->pDynamicStates + info.pDynamicState->dynamicStateCount);
      bool changed = false;
      if (info.pDepthStencilState && info.pDepthStencilState->stencilTestEnable) {
        e.states.push_back(VK_DYNAMIC_STATE_STENCIL_REFERENCE);
        e.states.push_back(VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK);
        e.states.push_back(VK_DYNAMIC_STATE_STENCIL_WRITE_MASK);
        changed = true;
      }
      if (info.pColorBlendState) {
        const auto* blend = info.pColorBlendState;
        for (uint32_t a = 0; a < blend->attachmentCount; ++a) {
          if (UsesBlendConstant(blend->pAttachments[a])) {
            e.states.push_back(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
            changed = true;
            break;
          }
        }
      }
      if (changed) {
        e.dynamic = *info.pDynamicState;
        e.dynamic.dynamicStateCount = uint32_t(e.states.size());
        e.dynamic.pDynamicStates = e.states.data();
        info.pDynamicState = &e.dynamic;
      }
    }
    if (info.pVertexInputState) {
      const auto* vertex = info.pVertexInputState;
      bool rewrite = false;
      for (uint32_t a = 0; a < vertex->vertexAttributeDescriptionCount; ++a) {
        rewrite |= LayerVertexFormat(vertex->pVertexAttributeDescriptions[a].format) !=
                   vertex->pVertexAttributeDescriptions[a].format;
      }
      if (rewrite) {
        e.attributes.assign(vertex->pVertexAttributeDescriptions,
                            vertex->pVertexAttributeDescriptions + vertex->vertexAttributeDescriptionCount);
        for (auto& attribute : e.attributes) attribute.format = LayerVertexFormat(attribute.format);
        e.vertex = *vertex;
        e.vertex.pVertexAttributeDescriptions = e.attributes.data();
        info.pVertexInputState = &e.vertex;
      }
    }
  }
  return CreatePipelines(device, count, list.data(), allocator, pipelines, layer.create_graphics);
}

VKAPI_ATTR VkResult VKAPI_CALL LayerCreateComputePipelines(VkDevice device, VkPipelineCache, uint32_t count,
                                                           const VkComputePipelineCreateInfo* infos,
                                                           const VkAllocationCallbacks* allocator,
                                                           VkPipeline* pipelines) {
  return CreatePipelines(device, count, infos, allocator, pipelines, layer.create_compute);
}

VKAPI_ATTR VkResult VKAPI_CALL LayerCreateBuffer(VkDevice device, const VkBufferCreateInfo* info,
                                                 const VkAllocationCallbacks* allocator, VkBuffer* buffer) {
  if (!(info->usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
    return layer.create_buffer(device, info, allocator, buffer);
  }
  VkBufferCreateInfo edited = *info;
  edited.usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  if (layer.conditional) edited.usage |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
  if (layer.buffer_address.load(std::memory_order_relaxed)) {
    edited.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  }
  return layer.create_buffer(device, &edited, allocator, buffer);
}

VKAPI_ATTR VkResult VKAPI_CALL LayerQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits,
                                                VkFence fence) {
  // Plume makes every submit wait for its semaphores at COLOR_ATTACHMENT_OUTPUT.
  // The native present uploads through TRANSFER before it touches the colour
  // output, and a wait that late does not protect those writes: wait for
  // everything.
  thread_local std::vector<VkSubmitInfo> edited;
  thread_local std::vector<VkPipelineStageFlags> stages;
  edited.assign(submits, submits + count);
  uint32_t most = 0;
  for (const VkSubmitInfo& submit : edited) most = std::max(most, submit.waitSemaphoreCount);
  stages.assign(most, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  for (VkSubmitInfo& submit : edited) {
    if (submit.waitSemaphoreCount) submit.pWaitDstStageMask = stages.data();
  }
  const VkResult result = layer.queue_submit(queue, count, edited.data(), fence);
  if (result == VK_ERROR_DEVICE_LOST) layer.device_lost.store(true);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL LayerWaitForFences(VkDevice device, uint32_t count, const VkFence* fences,
                                                  VkBool32 wait_all, uint64_t timeout) {
  const VkResult result = layer.wait_for_fences(device, count, fences, wait_all, timeout);
  if (result == VK_ERROR_DEVICE_LOST) layer.device_lost.store(true);
  return result;
}

}  // namespace

void InstallVulkanLayer() {
  if (layer.installed) return;
  layer.installed = true;
  layer.create_device = vkCreateDevice;
  layer.create_graphics = vkCreateGraphicsPipelines;
  layer.create_compute = vkCreateComputePipelines;
  layer.create_buffer = vkCreateBuffer;
  layer.queue_submit = vkQueueSubmit;
  layer.wait_for_fences = vkWaitForFences;
  vkCreateDevice = LayerCreateDevice;
  vkCreateGraphicsPipelines = LayerCreateGraphicsPipelines;
  vkCreateComputePipelines = LayerCreateComputePipelines;
  vkCreateBuffer = LayerCreateBuffer;
  vkQueueSubmit = LayerQueueSubmit;
  vkWaitForFences = LayerWaitForFences;
}

void VulkanLayerDeviceCreated(bool buffer_device_address) {
  layer.buffer_address.store(buffer_device_address);
}

bool PlumeDeviceLost() { return layer.device_lost.load(); }

bool PlumeConditionalRenderingSupported() { return layer.conditional; }

}  // namespace renut
