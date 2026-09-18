#include "plume_renderer.h"

#include "plume_vulkan.h"

#include <array>
#include <atomic>
#include <mutex>
#include <sstream>
#include <stdexcept>
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
  std::unique_ptr<RenderCommandList> commands;
  std::unique_ptr<RenderCommandFence> fence;
  std::unique_ptr<RenderCommandSemaphore> acquire;
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
    acquire.reset();
    fence.reset();
    commands.reset();
    swap.reset();
    queue.reset();
    device.reset();
    api.reset();
    if (window) ANativeWindow_release(window);
  }
};

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
    s.commands = s.queue->createCommandList();
    s.fence = s.device->createCommandFence();
    s.acquire = s.device->createCommandSemaphore();
    for (uint32_t i = 0; i < s.swap->getTextureCount(); ++i) {
      s.release.push_back(s.device->createCommandSemaphore());
    }
    Require(s.commands && s.fence && s.acquire, "Plume: falha ao alocar a apresentação.");
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
  uint32_t index = 0;
  if (!s.swap->acquireTexture(s.acquire.get(), &index)) {
    s.swap->resize();
    return;
  }
  auto* target = s.swap->getTexture(index);
  s.commands->begin();
  s.commands->barriers(RenderBarrierStage::COPY,
                       RenderTextureBarrier(target, RenderTextureLayout::COPY_DEST));
  VkClearColorValue value{};
  for (int i = 0; i < 4; ++i) value.float32[i] = color[i];
  VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdClearColorImage(static_cast<VulkanCommandList*>(s.commands.get())->vk,
                       static_cast<VulkanTexture*>(target)->vk,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
  s.commands->barriers(RenderBarrierStage::NONE,
                       RenderTextureBarrier(target, RenderTextureLayout::PRESENT));
  s.commands->end();
  const RenderCommandList* list = s.commands.get();
  auto* wait = s.acquire.get();
  auto* signal = s.release[index].get();
  s.queue->executeCommandLists(&list, 1, &wait, 1, &signal, 1, s.fence.get());
  s.swap->present(index, &signal, 1);
  s.queue->waitForCommandFence(s.fence.get());
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
