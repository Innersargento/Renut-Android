#include "plume_renderer.h"

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

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rex/logging.h>

void* renutXenosVulkanSymbol(const char* name);
void renutSetPlumeVulkanEntry(PFN_vkGetInstanceProcAddr entry);

namespace renut {
using namespace plume;

namespace {
std::mutex instance_mutex;
std::unique_ptr<PlumeRenderer> instance;

void Require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
}  // namespace

struct PlumeRenderer::State {
  ANativeWindow* window = nullptr;
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
    if (window) ANativeWindow_release(window);
  }
};

namespace {

// The pipeline cache kept on disk between runs. Upstream Plume passes no cache,
// which makes the driver recompile every pipeline on every launch: the first
// pass through Showdown Town spent 67% of its frame time inside
// vkCreateGraphicsPipelines, and none of that work survived.
//
// Every pipeline the generated copy of plume_vulkan.cpp creates comes through
// renutCreateGraphicsPipelines below (the seam is in
// android/app/src/main/cpp/CMakeLists.txt). When the driver can be asked for a
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
  VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  info.initialDataSize = size;
  info.pInitialData = data;
  VkPipelineCache cache = VK_NULL_HANDLE;
  if (vkCreatePipelineCache(device, &info, nullptr, &cache) == VK_SUCCESS) {
    *bytes = size;
  } else {
    cache = CreateEmptyCache(device);
  }
  if (data) munmap(data, size);
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

PlumeRenderer::PlumeRenderer() : state_(std::make_unique<State>()) {}
PlumeRenderer::~PlumeRenderer() = default;

bool PlumeRenderer::Start(ANativeWindow* window, std::string* error) {
  std::lock_guard lock(instance_mutex);
  if (instance) return true;
  try {
    auto entry = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        renutXenosVulkanSymbol("vkGetInstanceProcAddr"));
    Require(entry, "Plume: carregador Vulkan da sessão indisponível.");
    renutSetPlumeVulkanEntry(entry);
    std::unique_ptr<PlumeRenderer> renderer(new PlumeRenderer());
    auto& s = *renderer->state_;
    s.window = window;
    ANativeWindow_acquire(window);
    s.api = std::make_unique<VulkanInterface>();
    Require(s.api->isValid(), "Plume: instância Vulkan inválida.");
    s.device = s.api->createDevice("");
    Require(bool(s.device), "Plume: dispositivo indisponível.");
    s.gpu = s.device->getDescription().name;
    s.queue = s.device->createCommandQueue(RenderCommandListType::DIRECT);
    Require(bool(s.queue), "Plume: fila indisponível.");
    // The Android surface reports RGBA8 on the tested devices; fall back to BGRA8.
    auto* device = static_cast<VulkanDevice*>(s.device.get());
    VkAndroidSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    info.window = window;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    Require(vkCreateAndroidSurfaceKHR(s.api->instance, &info, nullptr, &surface) == VK_SUCCESS,
            "Plume: superfície indisponível.");
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
    s.swap = s.queue->createSwapChain(RenderSwapChainDesc(window, s.format, 3));
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
      pipeline_cache.lookup = s.device->getCapabilities().pipelineCacheLookup;
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

bool PlumeRenderer::AttachWindow(ANativeWindow* window) {
  auto& s = *state_;
  if (!window) return false;
  // Waits for a present that was already under way when the surface died.
  std::lock_guard swap_lock(s.swap_mutex);
  try {
    ANativeWindow_acquire(window);
    // Everything queued before the pause has to finish before the old swapchain
    // goes: it may still hold images the presentation engine is showing.
    WaitForAllSlots();
    vkDeviceWaitIdle(static_cast<VulkanDevice*>(s.device.get())->vk);
    s.release.clear();
    s.swap.reset();
    if (s.window) ANativeWindow_release(s.window);
    s.window = window;
    s.swap = s.queue->createSwapChain(RenderSwapChainDesc(window, s.format, 3));
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

// The seams in the generated copy of plume_vulkan.cpp, in place of its
// vkCreateGraphicsPipelines and vkCreateComputePipelines calls (which pass no
// cache).
VkResult renutCreateGraphicsPipelines(VkDevice device, VkPipelineCache, uint32_t count,
                                      const VkGraphicsPipelineCreateInfo* infos,
                                      const VkAllocationCallbacks* allocator, VkPipeline* pipelines) {
  return CreatePipelines(device, count, infos, allocator, pipelines, vkCreateGraphicsPipelines);
}

VkResult renutCreateComputePipelines(VkDevice device, VkPipelineCache, uint32_t count,
                                     const VkComputePipelineCreateInfo* infos,
                                     const VkAllocationCallbacks* allocator, VkPipeline* pipelines) {
  return CreatePipelines(device, count, infos, allocator, pipelines, vkCreateComputePipelines);
}
