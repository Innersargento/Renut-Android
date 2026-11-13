#include "plume_renderer.h"

#include "frame_pacing.h"
#include "plume_vulkan.h"
#include "spirv_descriptor_sets.h"
#include "spirv_int64.h"
#include "spirv_position_varying.h"
#include "spirv_vertex_inputs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <unordered_map>
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

#include <rex/cvar.h>
#include <rex/logging.h>

// The session's own Vulkan loader (the bundled driver), opened by android_vulkan_loader.cpp.
void* renutXenosVulkanSymbol(const char* name);
void renutSetPlumeVulkanEntry(PFN_vkGetInstanceProcAddr entry);

REXCVAR_DEFINE_BOOL(renut_plume_vsync, true, "reNut/Plume",
                    "Present through the display's vertical sync (FIFO). Off asks for immediate "
                    "presentation where the driver has it, so frames are not held to the refresh rate");

namespace renut {
using namespace plume;

namespace {
// The platform window as Plume's swapchain wants it, and the reference the
// renderer keeps on it (the Android surface is reference counted).
RenderWindow ToRenderWindow(PlumeWindow* window) { return window; }
void AcquireWindow(PlumeWindow* window) { ANativeWindow_acquire(window); }
void ReleaseWindow(PlumeWindow* window) { ANativeWindow_release(window); }
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
  PFN_vkCreateImage create_image = nullptr;
  PFN_vkCreateImageView create_image_view = nullptr;
  PFN_vkCreateShaderModule create_shader_module = nullptr;
  PFN_vkDestroyShaderModule destroy_shader_module = nullptr;
  PFN_vkQueueSubmit queue_submit = nullptr;
  PFN_vkWaitForFences wait_for_fences = nullptr;
  bool installed = false;
  bool cache_control = false;               // VK_EXT_pipeline_creation_cache_control is on
  std::atomic<bool> buffer_address{false};  // buffers get the device address usage
  // The device takes the *_USCALED / *_SSCALED formats as vertex attributes (Turnip does; Qualcomm's own driver
  // has none of them, and compiles no vertex shader whose float input is fed an integer attribute). Known once
  // the device is made; without them the vertex modules are retyped (TypedVertexModule).
  std::atomic<bool> scaled_vertex{true};
  // The device has no 64-bit integers in shaders (shaderInt64; Qualcomm's own driver): the modules that make
  // their buffer addresses of them are rewritten for pairs of 32-bit ones (spirv_int64.h).
  std::atomic<bool> lower_int64{false};
  std::atomic<uint64_t> lowered{0}, not_lowered{0};
  // Qualcomm's own driver (VK_DRIVER_ID_QUALCOMM_PROPRIETARY). Its geometry shaders read the vertex position
  // from a location, which the layer has the vertex modules of their pipelines write (PositionedVertexModule).
  std::atomic<bool> qualcomm{false};
  // The device's limits do not take the bindless heaps (the Mali-G52): the recompiled modules are rewritten for
  // a set of each draw's own textures (spirv_descriptor_sets.h, GpuBackend::DrawTextureSet).
  std::atomic<bool> per_draw_textures{false};
  std::atomic<uint64_t> per_draw_rewritten{0};
  // The device samples ETC2 but not BC: images and views of the BC formats are made in the ETC2/EAC format of
  // the same block size (EtcFormatOfBc), which the textures were transcoded to.
  std::atomic<bool> bc_as_etc2{false};
  // What the pipeline cache's file name carries for a driver other than Turnip ("-drv<id>"), set with the device.
  std::string cache_tag;
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
    // A cache per driver: each one refuses the other's blob, and the first fold under one would write the
    // other's file over with its own. Turnip keeps the name it always had. The device exists by now (the
    // renderer starts before the path is set).
    std::string tagged = path;
    const std::string suffix = ".bin";
    if (!layer.cache_tag.empty() && tagged.size() > suffix.size() &&
        tagged.compare(tagged.size() - suffix.size(), suffix.size(), suffix) == 0) {
      tagged.insert(tagged.size() - suffix.size(), layer.cache_tag);
    }
    pipeline_cache.path = tagged;
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

bool PlumeScaledVertexFormats() { return layer.scaled_vertex.load(std::memory_order_relaxed); }

bool PlumeQualcommDriver() { return layer.qualcomm.load(std::memory_order_relaxed); }
bool PlumePerDrawTextures() { return layer.per_draw_textures.load(std::memory_order_relaxed); }
void PlumeSetBcAsEtc2(bool on) { layer.bc_as_etc2.store(on, std::memory_order_relaxed); }

PlumePipelineLookupOnly::PlumePipelineLookupOnly() { pipeline_lookup_only = true; }
PlumePipelineLookupOnly::~PlumePipelineLookupOnly() { pipeline_lookup_only = false; }

void PlumePipelineCacheCounts(uint64_t* found, uint64_t* compiled) {
  *found = pipeline_cache.found.load(std::memory_order_relaxed);
  *compiled = pipeline_cache.compiled.load(std::memory_order_relaxed);
}

void InstallVulkanLayer();
void VulkanLayerDeviceCreated(bool buffer_device_address);
namespace gpu {  // gpu/core/context.cpp
bool NeedsPerDrawTextures(const VkPhysicalDeviceLimits& limits);
std::string MissingDescriptorLimits(const VkPhysicalDeviceLimits& limits, const VkPhysicalDeviceFeatures& features);
}

PlumeRenderer::PlumeRenderer() : state_(std::make_unique<State>()) {}
PlumeRenderer::~PlumeRenderer() = default;

bool PlumeRenderer::IsAvailable() {
  return renutXenosVulkanSymbol("vkGetInstanceProcAddr") != nullptr;
}

bool PlumeRenderer::Start(PlumeWindow* window, std::string* error) {
  std::lock_guard lock(instance_mutex);
  if (instance) return true;
  if (!IsAvailable()) {
    const char* why = "Plume: nenhum carregador Vulkan neste sistema.";
    if (error) *error = why;
    REXLOG_ERROR("Renut: {}", why);
    return false;
  }
  try {
    auto entry = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        renutXenosVulkanSymbol("vkGetInstanceProcAddr"));
    Require(entry, "Plume: carregador Vulkan da sessão indisponível.");
    renutSetPlumeVulkanEntry(entry);
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
    Require(bool(s.device) && static_cast<VulkanDevice*>(s.device.get())->vk != VK_NULL_HANDLE,
            "Plume: dispositivo indisponível.");
    {
      // Plume calls the Vulkan 1.2 name, and volk resolved it from the instance only. A Vulkan 1.1
      // driver (Mali-G52 r34p0 on the Unisoc T618) has buffer device addresses as
      // VK_KHR_buffer_device_address alone: the core name is no entry point there and the first
      // call jumped to address 0. Asked of the device, under either name.
      const VkDevice vk_device = static_cast<VulkanDevice*>(s.device.get())->vk;
      auto address = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(
          vkGetDeviceProcAddr(vk_device, "vkGetBufferDeviceAddress"));
      if (!address) {
        address = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(
            vkGetDeviceProcAddr(vk_device, "vkGetBufferDeviceAddressKHR"));
      }
      vkGetBufferDeviceAddress = address;
      Require(address || !s.device->getCapabilities().bufferDeviceAddress,
              "Plume: vkGetBufferDeviceAddress indisponível.");
    }
    VulkanLayerDeviceCreated(s.device->getCapabilities().bufferDeviceAddress);
    s.gpu = s.device->getDescription().name;
    // Past the bindless heaps' limits each draw binds a set of its own textures; past even that, the
    // start is refused here with the reason on screen: past its limits the driver may fault in a loop (the
    // guest fault handler takes the signal) and the game stays on a black screen.
    const auto* vulkan_device = static_cast<VulkanDevice*>(s.device.get());
    const VkPhysicalDeviceLimits& limits = vulkan_device->physicalDeviceProperties.limits;
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(vulkan_device->physicalDevice, &features);
    layer.per_draw_textures.store(gpu::NeedsPerDrawTextures(limits), std::memory_order_relaxed);
    if (layer.per_draw_textures.load(std::memory_order_relaxed)) {
      REXLOG_INFO("Renut: Plume {}: the bindless texture heaps are past its limits ({} sets, {} textures and {} "
                  "samplers a stage); each draw binds a set of its own textures",
                  s.gpu, limits.maxBoundDescriptorSets, limits.maxPerStageDescriptorSampledImages,
                  limits.maxPerStageDescriptorSamplers);
    }
    if (const std::string missing = gpu::MissingDescriptorLimits(limits, features); !missing.empty()) {
      const std::string why = "Plume: a GPU " + s.gpu + " não comporta o modelo de texturas do renderizador (" +
                              missing + ").";
      REXLOG_ERROR("Renut: {}", why);
      Require(false, why.c_str());
    }
    s.queue = s.device->createCommandQueue(RenderCommandListType::DIRECT);
    Require(bool(s.queue), "Plume: fila indisponível.");
    // The Android surface reports RGBA8 on the tested devices; fall back to BGRA8.
    auto* device = static_cast<VulkanDevice*>(s.device.get());
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkAndroidSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    info.window = window;
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
// The vertex modules of a device without the scaled vertex formats: the code of each as Plume made it, and the
// forms of it whose inputs are the integer vectors its attributes are (spirv_vertex_inputs.h), by the kinds of
// its 32 locations.
struct VertexModules {
  std::mutex mutex;
  // Every vertex module's code, the retyped forms' included (by their own handle).
  std::unordered_map<uint64_t, std::vector<uint32_t>> code;
  std::map<std::pair<uint64_t, std::string>, VkShaderModule> typed;
  // The form of a module (original or retyped) that also writes its position at
  // spirv::kPositionVaryingLocation, for the pipelines with a geometry shader.
  std::unordered_map<uint64_t, VkShaderModule> positioned;
  uint64_t made = 0, refused = 0, positioned_made = 0, positioned_refused = 0;
};
VertexModules vertex_modules;

uint64_t ModuleKey(VkShaderModule module) { return uint64_t(reinterpret_cast<uintptr_t>(module)); }

// 0 not an integer format, 1 signed, 2 unsigned.
uint32_t IntegerVertexFormat(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32B32_SINT:
    case VK_FORMAT_R32G32B32A32_SINT:
      return 1;
    case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32B32_UINT:
    case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32:
      return 2;
    default:
      return 0;
  }
}

// The form of `module` for these kinds, made once; null when it is not a vertex module the layer saw made or
// its inputs could not be retyped (the pipeline is then asked for as it was).
VkShaderModule TypedVertexModule(VkDevice device, VkShaderModule module, const uint8_t* kinds) {
  VertexModules& m = vertex_modules;
  std::lock_guard lock(m.mutex);
  const auto code = m.code.find(ModuleKey(module));
  if (code == m.code.end()) return VK_NULL_HANDLE;
  const auto key = std::make_pair(ModuleKey(module), std::string(reinterpret_cast<const char*>(kinds), 32));
  const auto found = m.typed.find(key);
  if (found != m.typed.end()) return found->second;
  VkShaderModule typed = VK_NULL_HANDLE;
  std::vector<uint32_t> words;
  if (spirv::RetypeVertexInputs(code->second, kinds, words)) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = words.size() * sizeof(uint32_t);
    info.pCode = words.data();
    if (layer.create_shader_module(device, &info, nullptr, &typed) != VK_SUCCESS) typed = VK_NULL_HANDLE;
  }
  if (typed != VK_NULL_HANDLE) {
    if (++m.made == 1) REXLOG_INFO("Renut: Plume vertex modules are retyped for their integer attributes");
    m.code[ModuleKey(typed)] = std::move(words);
  } else if (++m.refused <= 8) {
    REXLOG_WARN("Renut: Plume vertex module not retyped for its integer attributes");
  }
  m.typed.emplace(key, typed);
  return typed;
}

// The form of `module` (a vertex module the layer saw made, or a retyped one) that also writes its position at
// spirv::kPositionVaryingLocation, where the host's geometry shaders read it; made once, null when it cannot be.
VkShaderModule PositionedVertexModule(VkDevice device, VkShaderModule module) {
  VertexModules& m = vertex_modules;
  std::lock_guard lock(m.mutex);
  const auto found = m.positioned.find(ModuleKey(module));
  if (found != m.positioned.end()) return found->second;
  VkShaderModule positioned = VK_NULL_HANDLE;
  const auto code = m.code.find(ModuleKey(module));
  std::vector<uint32_t> words;
  if (code != m.code.end() && spirv::AddPositionVarying(code->second, words)) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = words.size() * sizeof(uint32_t);
    info.pCode = words.data();
    if (layer.create_shader_module(device, &info, nullptr, &positioned) != VK_SUCCESS) positioned = VK_NULL_HANDLE;
  }
  if (positioned != VK_NULL_HANDLE) {
    if (++m.positioned_made == 1) {
      REXLOG_INFO("Renut: Plume vertex modules of geometry-shader pipelines hand their position on at location {}",
                  spirv::kPositionVaryingLocation);
    }
  } else if (++m.positioned_refused <= 8) {
    REXLOG_WARN("Renut: Plume vertex module without its position at location {}: its geometry shader reads none",
                spirv::kPositionVaryingLocation);
  }
  m.positioned.emplace(ModuleKey(module), positioned);
  return positioned;
}

VKAPI_ATTR VkResult VKAPI_CALL LayerCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* info,
                                                       const VkAllocationCallbacks* allocator,
                                                       VkShaderModule* module) {
  VkShaderModuleCreateInfo edited = *info;
  std::vector<uint32_t> per_draw, lowered;
  if (layer.per_draw_textures.load(std::memory_order_relaxed) && info->pCode &&
      spirv::RewritePerDrawTextures(info->pCode, info->codeSize / sizeof(uint32_t), per_draw)) {
    edited.pCode = per_draw.data();
    edited.codeSize = per_draw.size() * sizeof(uint32_t);
    if (layer.per_draw_rewritten.fetch_add(1) == 0) {
      REXLOG_INFO("Renut: Plume shader modules are rewritten for a texture set per draw (sets 0 and 1)");
    }
  }
  if (layer.lower_int64.load(std::memory_order_relaxed) && edited.pCode &&
      spirv::UsesInt64(edited.pCode, edited.codeSize / sizeof(uint32_t))) {
    if (spirv::LowerInt64(edited.pCode, edited.codeSize / sizeof(uint32_t), lowered)) {
      edited.pCode = lowered.data();
      edited.codeSize = lowered.size() * sizeof(uint32_t);
      if (layer.lowered.fetch_add(1) == 0) {
        REXLOG_INFO("Renut: Plume shader modules with 64-bit addresses are rewritten for pairs of 32-bit integers");
      }
    } else if (layer.not_lowered.fetch_add(1) < 8) {
      REXLOG_WARN("Renut: Plume shader module with 64-bit integers not rewritten ({} words)",
                  info->codeSize / sizeof(uint32_t));
    }
  }
  const VkResult result = layer.create_shader_module(device, &edited, allocator, module);
  // The vertex modules' code is kept where forms of them are made: the retyped ones (a device without scaled
  // formats) and the positioned ones (Qualcomm's own driver: its geometry shaders read the position from a
  // location).
  if (result == VK_SUCCESS && edited.pCode &&
      (!layer.scaled_vertex.load(std::memory_order_relaxed) || layer.qualcomm.load(std::memory_order_relaxed)) &&
      spirv::IsVertexModule(edited.pCode, edited.codeSize / sizeof(uint32_t))) {
    std::lock_guard lock(vertex_modules.mutex);
    vertex_modules.code[ModuleKey(*module)].assign(edited.pCode, edited.pCode + edited.codeSize / sizeof(uint32_t));
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL LayerDestroyShaderModule(VkDevice device, VkShaderModule module,
                                                    const VkAllocationCallbacks* allocator) {
  {
    VertexModules& m = vertex_modules;
    std::lock_guard lock(m.mutex);
    // A form's positioned form goes with it.
    auto drop_positioned = [&](uint64_t key) {
      const auto p = m.positioned.find(key);
      if (p == m.positioned.end()) return;
      if (p->second != VK_NULL_HANDLE) layer.destroy_shader_module(device, p->second, nullptr);
      m.positioned.erase(p);
    };
    if (m.code.erase(ModuleKey(module))) {
      drop_positioned(ModuleKey(module));
      for (auto it = m.typed.lower_bound({ModuleKey(module), std::string()});
           it != m.typed.end() && it->first.first == ModuleKey(module);) {
        if (it->second != VK_NULL_HANDLE) {
          m.code.erase(ModuleKey(it->second));
          drop_positioned(ModuleKey(it->second));
          layer.destroy_shader_module(device, it->second, nullptr);
        }
        it = m.typed.erase(it);
      }
    }
  }
  layer.destroy_shader_module(device, module, allocator);
}

VkFormat LayerVertexFormat(VkFormat format) {
  // A device without the scaled formats keeps the integer ones, and the vertex module is given integer inputs
  // that it converts itself (TypedVertexModule). The unsigned scaled 2_10_10_10 is then the packed integer.
  if (!layer.scaled_vertex.load(std::memory_order_relaxed)) {
    switch (format) {
      case VK_FORMAT_R8G8_UINT: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
      case VK_FORMAT_R8G8_SINT: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
      default: return format;
    }
  }
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
  bool control_wanted = false;
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
  if (control_wanted) {
    edited.pNext = chain;
    edited.enabledExtensionCount = uint32_t(extensions.size());
    edited.ppEnabledExtensionNames = extensions.data();
  }
  // What the device makes of the vertex formats the renderer would use, and which driver it is.
  {
    bool scaled = true;
    for (const VkFormat format :
         {VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16_SSCALED,
          VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_R16G16B16A16_SSCALED, VK_FORMAT_A2B10G10R10_USCALED_PACK32}) {
      VkFormatProperties properties{};
      vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
      scaled = scaled && (properties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
    }
    layer.scaled_vertex.store(scaled, std::memory_order_relaxed);
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(physical, &features);
    layer.lower_int64.store(!features.shaderInt64, std::memory_order_relaxed);
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &driver;
    vkGetPhysicalDeviceProperties2(physical, &properties);
    layer.cache_tag =
        driver.driverID == VK_DRIVER_ID_MESA_TURNIP ? std::string() : fmt::format("-drv{}", uint32_t(driver.driverID));
    layer.qualcomm.store(driver.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY, std::memory_order_relaxed);
    REXLOG_INFO("Renut: Plume Vulkan driver {} ({}, {}): scaled vertex formats {}; 64-bit integers in shaders {}",
                uint32_t(driver.driverID), driver.driverName, driver.driverInfo,
                scaled ? "taken by the device" : "missing, the vertex modules get integer inputs",
                features.shaderInt64 ? "available" : "missing, addresses become pairs of 32-bit integers");
  }
  const VkResult result = layer.create_device(physical, &edited, allocator, device);
  layer.cache_control = result == VK_SUCCESS && control_wanted;
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
    std::vector<VkPipelineShaderStageCreateInfo> stages;
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
    // A device without the scaled formats: the vertex module in the form whose inputs are the integers these
    // attributes are. One the backend flagged (specialization constant 2, g_IntInputs: the shader reads its
    // bits with asint) keeps its bits; the others are converted to the numbers a scaled format would have given.
    if (!layer.scaled_vertex.load(std::memory_order_relaxed) && info.pVertexInputState && info.pStages) {
      uint32_t vertex_stage = UINT32_MAX;
      for (uint32_t s = 0; s < info.stageCount; ++s) {
        if (info.pStages[s].stage == VK_SHADER_STAGE_VERTEX_BIT) vertex_stage = s;
      }
      if (vertex_stage != UINT32_MAX) {
        const VkPipelineShaderStageCreateInfo& stage = info.pStages[vertex_stage];
        uint32_t read_as_bits = 0;
        if (const VkSpecializationInfo* spec = stage.pSpecializationInfo) {
          for (uint32_t c = 0; c < spec->mapEntryCount; ++c) {
            const VkSpecializationMapEntry& entry = spec->pMapEntries[c];
            if (entry.constantID == 2 && entry.size == sizeof(uint32_t) && entry.offset + entry.size <= spec->dataSize) {
              std::memcpy(&read_as_bits, static_cast<const uint8_t*>(spec->pData) + entry.offset, sizeof(uint32_t));
            }
          }
        }
        uint8_t kinds[32] = {};
        bool any = false;
        const auto* vertex = info.pVertexInputState;
        for (uint32_t a = 0; a < vertex->vertexAttributeDescriptionCount; ++a) {
          const VkVertexInputAttributeDescription& attribute = vertex->pVertexAttributeDescriptions[a];
          const uint32_t integer = IntegerVertexFormat(attribute.format);
          if (!integer || attribute.location >= 32) continue;
          const bool bits = (read_as_bits >> attribute.location) & 1;
          kinds[attribute.location] = integer == 1 ? (bits ? spirv::kInputSignedBits : spirv::kInputSignedNumber)
                                                   : (bits ? spirv::kInputUnsignedBits : spirv::kInputUnsignedNumber);
          any = true;
        }
        if (any) {
          if (const VkShaderModule typed = TypedVertexModule(device, stage.module, kinds); typed != VK_NULL_HANDLE) {
            e.stages.assign(info.pStages, info.pStages + info.stageCount);
            e.stages[vertex_stage].module = typed;
            info.pStages = e.stages.data();
          }
        }
      }
    }
    // Qualcomm's own driver, a pipeline with a geometry shader (the host's rect_list_position.geom /
    // point_list_position.geom): its vertex module in the form that also writes the position at the location
    // those read it from (spirv_position_varying.h).
    if (layer.qualcomm.load(std::memory_order_relaxed) && info.pStages) {
      uint32_t vertex_stage = UINT32_MAX;
      bool geometry = false;
      for (uint32_t s = 0; s < info.stageCount; ++s) {
        if (info.pStages[s].stage == VK_SHADER_STAGE_VERTEX_BIT) vertex_stage = s;
        geometry |= info.pStages[s].stage == VK_SHADER_STAGE_GEOMETRY_BIT;
      }
      if (geometry && vertex_stage != UINT32_MAX) {
        if (const VkShaderModule positioned = PositionedVertexModule(device, info.pStages[vertex_stage].module);
            positioned != VK_NULL_HANDLE) {
          if (info.pStages != e.stages.data()) e.stages.assign(info.pStages, info.pStages + info.stageCount);
          e.stages[vertex_stage].module = positioned;
          info.pStages = e.stages.data();
        }
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
  if (layer.buffer_address.load(std::memory_order_relaxed)) {
    edited.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  }
  return layer.create_buffer(device, &edited, allocator, buffer);
}

// A device that samples ETC2 but not BC (PlumeBcAsEtc2): the BC formats Plume makes images and views of hold
// the ETC2/EAC blocks of the same size the textures were transcoded to (etc2_transcode.h).
VkFormat EtcFormatOfBc(VkFormat format) {
  if (!layer.bc_as_etc2.load(std::memory_order_relaxed)) return format;
  switch (format) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK;
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK: return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
    case VK_FORMAT_BC4_UNORM_BLOCK: return VK_FORMAT_EAC_R11_UNORM_BLOCK;
    case VK_FORMAT_BC5_UNORM_BLOCK: return VK_FORMAT_EAC_R11G11_UNORM_BLOCK;
    default: return format;
  }
}

VKAPI_ATTR VkResult VKAPI_CALL LayerCreateImage(VkDevice device, const VkImageCreateInfo* info,
                                                const VkAllocationCallbacks* allocator, VkImage* image) {
  const VkFormat format = EtcFormatOfBc(info->format);
  if (format == info->format) return layer.create_image(device, info, allocator, image);
  VkImageCreateInfo edited = *info;
  edited.format = format;
  return layer.create_image(device, &edited, allocator, image);
}

VKAPI_ATTR VkResult VKAPI_CALL LayerCreateImageView(VkDevice device, const VkImageViewCreateInfo* info,
                                                    const VkAllocationCallbacks* allocator, VkImageView* view) {
  const VkFormat format = EtcFormatOfBc(info->format);
  if (format == info->format) return layer.create_image_view(device, info, allocator, view);
  VkImageViewCreateInfo edited = *info;
  edited.format = format;
  return layer.create_image_view(device, &edited, allocator, view);
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
  layer.create_image = vkCreateImage;
  layer.create_image_view = vkCreateImageView;
  vkCreateImage = LayerCreateImage;
  vkCreateImageView = LayerCreateImageView;
  layer.create_shader_module = vkCreateShaderModule;
  layer.destroy_shader_module = vkDestroyShaderModule;
  layer.queue_submit = vkQueueSubmit;
  layer.wait_for_fences = vkWaitForFences;
  vkCreateDevice = LayerCreateDevice;
  vkCreateGraphicsPipelines = LayerCreateGraphicsPipelines;
  vkCreateComputePipelines = LayerCreateComputePipelines;
  vkCreateBuffer = LayerCreateBuffer;
  vkCreateShaderModule = LayerCreateShaderModule;
  vkDestroyShaderModule = LayerDestroyShaderModule;
  vkQueueSubmit = LayerQueueSubmit;
  vkWaitForFences = LayerWaitForFences;
}

void VulkanLayerDeviceCreated(bool buffer_device_address) {
  layer.buffer_address.store(buffer_device_address);
}

bool PlumeDeviceLost() { return layer.device_lost.load(); }


}  // namespace renut
