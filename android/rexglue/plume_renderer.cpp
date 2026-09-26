#include "plume_renderer.h"

#include "plume_vulkan.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

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

// The generated copy of plume_vulkan.cpp asks for this cache on every pipeline
// it creates; the seam is in android/app/src/main/cpp/CMakeLists.txt. Upstream
// passes VK_NULL_HANDLE, which makes the driver recompile every pipeline on
// every launch: the first pass through Showdown Town spent 67% of its frame
// time inside vkCreateGraphicsPipelines, and none of that work survived.
std::mutex pipeline_cache_mutex;
std::string pipeline_cache_path;
VkDevice pipeline_cache_device = VK_NULL_HANDLE;
VkPipelineCache pipeline_cache = VK_NULL_HANDLE;

}  // namespace

void SetPlumePipelineCachePath(const std::string& path) {
  std::lock_guard lock(pipeline_cache_mutex);
  pipeline_cache_path = path;
}

namespace {
void WritePlumePipelineCache() {
  std::lock_guard lock(pipeline_cache_mutex);
  if (pipeline_cache == VK_NULL_HANDLE || pipeline_cache_path.empty()) return;
  size_t size = 0;
  if (vkGetPipelineCacheData(pipeline_cache_device, pipeline_cache, &size, nullptr) != VK_SUCCESS ||
      !size) {
    return;
  }
  std::vector<uint8_t> data(size);
  if (vkGetPipelineCacheData(pipeline_cache_device, pipeline_cache, &size, data.data()) !=
      VK_SUCCESS) {
    return;
  }
  // Write beside the target and rename: Android kills the process without
  // warning, and a truncated blob is one the driver rejects on the next run.
  const std::string temporary = pipeline_cache_path + ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(size));
    if (!out) return;
  }
  std::error_code error;
  std::filesystem::rename(temporary, pipeline_cache_path, error);
}

std::atomic<bool> pipeline_cache_saving{false};
std::chrono::steady_clock::time_point pipeline_cache_saved{};
}  // namespace

// On a thread of its own, at most once a minute. The blob grows past 300 MB,
// and writing it from the command list thread - after every report interval
// that compiled a pipeline - stalled the game for seconds at a time and was
// the thread's largest cost while a new area was being seen. A pipeline cache
// is internally synchronized, so reading it here while that thread keeps
// creating pipelines is allowed.
void SavePlumePipelineCache() {
  const auto now = std::chrono::steady_clock::now();
  if (pipeline_cache_saved.time_since_epoch().count() && now - pipeline_cache_saved < std::chrono::seconds(60)) {
    return;
  }
  bool idle = false;
  if (!pipeline_cache_saving.compare_exchange_strong(idle, true)) return;
  pipeline_cache_saved = now;
  std::thread([] {
    WritePlumePipelineCache();
    pipeline_cache_saving = false;
  }).detach();
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
  auto& slot = s.slots[slot_index % kPlumeFramesInFlight];
  // The caller waits before it reuses a slot, so this one is free by now.
  WaitForSlot(slot_index);
  uint32_t index = 0;
  bool acquired = s.swap->acquireTexture(slot.acquire.get(), &index);
  if (!acquired) {
    // Still run the frame: the guest's fences and resolved textures depend on it.
    if (frame) {
      const RenderCommandList* list = frame;
      s.queue->executeCommandLists(&list, 1, nullptr, 0, nullptr, 0, slot.fence.get());
      slot.pending = true;
    }
    s.swap->resize();
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

std::string PlumeRenderer::report() const {
  std::ostringstream out;
  out << "Plume: " << state_->gpu << ", quadros apresentados: " << state_->frames.load();
  return out.str();
}

}  // namespace renut

// Called by the generated copy of plume_vulkan.cpp, on the thread that creates
// pipelines. Seeded from the last run's blob; one the driver does not recognise
// (another device or driver version) is refused and the cache starts empty.
VkPipelineCache renutVulkanPipelineCache(VkDevice device) {
  std::lock_guard lock(renut::pipeline_cache_mutex);
  if (renut::pipeline_cache != VK_NULL_HANDLE && renut::pipeline_cache_device == device) {
    return renut::pipeline_cache;
  }
  std::vector<uint8_t> blob;
  if (!renut::pipeline_cache_path.empty()) {
    std::ifstream in(renut::pipeline_cache_path, std::ios::binary);
    if (in) blob.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  info.initialDataSize = blob.size();
  info.pInitialData = blob.empty() ? nullptr : blob.data();
  VkPipelineCache cache = VK_NULL_HANDLE;
  if (vkCreatePipelineCache(device, &info, nullptr, &cache) != VK_SUCCESS) {
    info.initialDataSize = 0;
    info.pInitialData = nullptr;
    if (vkCreatePipelineCache(device, &info, nullptr, &cache) != VK_SUCCESS) return VK_NULL_HANDLE;
  }
  REXLOG_INFO("Renut: Plume pipeline cache {} ({} KB from disk)",
              blob.empty() ? "criado" : "carregado", blob.size() >> 10);
  renut::pipeline_cache_device = device;
  renut::pipeline_cache = cache;
  return cache;
}
