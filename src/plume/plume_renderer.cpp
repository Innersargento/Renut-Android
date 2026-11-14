#include "plume_renderer.h"

#include "frame_pacing.h"
#include "plume_vulkan.h"
#include "plume/shaders/host_shaders.h"
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

REXCVAR_DEFINE_BOOL(renut_plume_present_pass, true, "reNut/Plume",
                    "Show the frame with one render pass on the swapchain image (cleared as the pass begins, the "
                    "frame drawn as a textured rectangle) instead of a clear and a blit of the whole image. Read "
                    "at start.");
REXCVAR_DEFINE_BOOL(renut_plume_present_fit, true, "reNut/Plume",
                    "Make the swapchain at the size of the game's frame (the screen's shape, the frame's lines) "
                    "and leave the scaling to the display's compositor, instead of filling a swapchain of the "
                    "screen's own resolution every frame. Read at start.");
REXCVAR_DEFINE_BOOL(renut_plume_present_prerotate, true, "reNut/Plume",
                    "Draw the frame already turned the way the screen is mounted (the swapchain's preTransform is "
                    "the surface's current one), so the compositor does not turn every frame itself, which on "
                    "many phones it does with the GPU. Needs renut_plume_present_pass. Read at start.");

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

// What Java last said about a surface's size (PlumeRenderer::SetSurfaceSize). The window
// is only compared, never used: it may be gone by the time a present reads this.
struct SurfaceSize {
  std::mutex mutex;
  const PlumeWindow* window = nullptr;
  uint32_t width = 0, height = 0;
  uint64_t serial = 0;
};
SurfaceSize surface_size;

const char* TransformName(VkSurfaceTransformFlagBitsKHR transform) {
  switch (transform) {
    case VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR: return "identity";
    case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR: return "rotate 90";
    case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: return "rotate 180";
    case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: return "rotate 270";
    case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_BIT_KHR: return "mirror";
    case VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR: return "inherit";
    default: return "mirror+rotate";
  }
}


void Require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

// What the layer's vkCreateSwapchainKHR makes of the swapchain Plume asks for. Plume asks for the window's size
// and no pre-transform; the renderer says here, before each resize, what the surface is and what it wants.
struct SwapchainPlan {
  // Asked (zero sizes leave Plume's request as it is):
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  uint32_t surface_w = 0, surface_h = 0;  // the surface, as the window is held
  uint32_t frame_w = 0, frame_h = 0;      // the frame the swapchain is made to fit (0: the surface's own size)
  bool prerotate = false;
  // Made:
  VkExtent2D extent{};  // of the images, turned with them
  VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;  // the preTransform given
  VkSurfaceTransformFlagBitsKHR current = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;    // the surface's, as it was then
  VkFormat format = VK_FORMAT_UNDEFINED;
};
SwapchainPlan swapchain_plan;

// The render pass that shows a frame (PlumeRenderer::SubmitAndPresent): raw Vulkan, as Plume's passes always
// load what the image held and a swapchain image is all written here.
struct PresentPass {
  bool ready = false;
  VkDevice device = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkRenderPass pass = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kPlumeFramesInFlight> sets{};
  // A view and a framebuffer per swapchain image, made at its first present and dropped with the swapchain.
  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
  };
  std::vector<Image> images;

  void DropImages() {
    for (Image& image : images) {
      if (image.framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device, image.framebuffer, nullptr);
      if (image.view != VK_NULL_HANDLE) vkDestroyImageView(device, image.view, nullptr);
    }
    images.clear();
  }
  void Destroy();
  const Image* ImageFor(VkImage image, VkFormat image_format, VkExtent2D extent);
};

// The constants of present.vert / present.frag.
struct PresentConstants {
  float turn[4];
  float extent[2];
  float uv_scale[2];
  float uv_limit[2];
};
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
  // FollowSurfaceSize: the last surface size it acted on, presents since the last check
  // of the surface's own extent, and the last mismatch it logged (logged once each).
  uint64_t size_serial = 0;
  uint32_t presents_since_check = 0;
  uint32_t logged_extent_w = 0, logged_extent_h = 0;
  // The letterboxed rectangle of the last present, logged when it changes.
  int32_t blit_rect[4] = {-1, -1, -1, -1};
  // The surface's size as the window is held: the window's own when it was attached, then what Java reports.
  uint32_t surface_w = 0, surface_h = 0;
  // The swapchain's images as the layer made them (SwapchainPlan), which is not what Plume believes when the
  // swapchain is fitted to the frame or turned.
  VkExtent2D extent{};
  VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  VkSurfaceTransformFlagBitsKHR surface_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  // renut_plume_present_*: as read at start. `samples` is what the callers of SubmitAndPresent go by.
  bool fit = false, prerotate = false, samples = false;
  PresentPass pass;
  // The frame the swapchain is fitted to, and the other size the last presents showed (the swapchain follows
  // once it has been shown for a while: the internal resolution option).
  uint32_t fit_w = 1280, fit_h = 720;
  uint32_t fit_next_w = 0, fit_next_h = 0, fit_count = 0;

  ~State() {
    if (device) {
      vkDeviceWaitIdle(static_cast<VulkanDevice*>(device.get())->vk);
    }
    release.clear();
    pass.Destroy();
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
  PFN_vkCreateSwapchainKHR create_swapchain = nullptr;
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
  // ARM's own driver (VK_DRIVER_ID_ARM_PROPRIETARY, the Mali-G52).
  std::atomic<bool> arm{false};
  // The device's limits do not take the bindless heaps (the Mali-G52): the recompiled modules are rewritten for
  // a set of each draw's own textures (spirv_descriptor_sets.h, GpuBackend::DrawTextureSet).
  std::atomic<bool> per_draw_textures{false};
  std::atomic<uint64_t> per_draw_rewritten{0};
  // The device samples ETC2 but not BC: images and views of the BC formats are made in the ETC2/EAC format of
  // the same block size (EtcFormatOfBc), which the textures were transcoded to.
  std::atomic<bool> bc_as_etc2{false};
  // What the pipeline cache's file name carries for a driver other than Turnip ("-drv<id>"), set with the device.
  std::string cache_tag;
  // "GPU name, driver name and info, Vulkan version" for the HUD's driver line, set with the device.
  std::mutex driver_text_mutex;
  std::string driver_text;
  std::atomic<bool> device_lost{false};
};
VulkanLayerState layer;

// The pass, the pipeline and what they need. The shader modules and the pipeline are made by the driver's own
// entry points: the layer's are for the modules and pipelines of the game's draws.
bool CreatePresentPass(PresentPass& p, VkDevice device, VkFormat format) {
  p.device = device;
  p.format = format;
  VkAttachmentDescription attachment{};
  attachment.format = format;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color;
  // The image is acquired by the time the submission's semaphore wait ends, which the layer puts at every stage
  // (LayerQueueSubmit); the frame's texture was written by the command list before this one.
  VkSubpassDependency dependency{};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.srcAccessMask = 0;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  pass_info.attachmentCount = 1;
  pass_info.pAttachments = &attachment;
  pass_info.subpassCount = 1;
  pass_info.pSubpasses = &subpass;
  pass_info.dependencyCount = 1;
  pass_info.pDependencies = &dependency;
  if (vkCreateRenderPass(device, &pass_info, nullptr, &p.pass) != VK_SUCCESS) return false;

  VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.maxAnisotropy = 1.0f;
  if (vkCreateSampler(device, &sampler_info, nullptr, &p.sampler) != VK_SUCCESS) return false;

  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 1;
  set_info.pBindings = &binding;
  if (vkCreateDescriptorSetLayout(device, &set_info, nullptr, &p.set_layout) != VK_SUCCESS) return false;
  VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PresentConstants)};
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &p.set_layout;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &range;
  if (vkCreatePipelineLayout(device, &layout_info, nullptr, &p.layout) != VK_SUCCESS) return false;

  VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kPlumeFramesInFlight};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = kPlumeFramesInFlight;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (vkCreateDescriptorPool(device, &pool_info, nullptr, &p.pool) != VK_SUCCESS) return false;
  std::array<VkDescriptorSetLayout, kPlumeFramesInFlight> layouts;
  layouts.fill(p.set_layout);
  VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool = p.pool;
  allocate.descriptorSetCount = kPlumeFramesInFlight;
  allocate.pSetLayouts = layouts.data();
  if (vkAllocateDescriptorSets(device, &allocate, p.sets.data()) != VK_SUCCESS) return false;

  const auto create_module = layer.create_shader_module ? layer.create_shader_module : vkCreateShaderModule;
  const auto destroy_module = layer.destroy_shader_module ? layer.destroy_shader_module : vkDestroyShaderModule;
  const auto create_pipelines = layer.create_graphics ? layer.create_graphics : vkCreateGraphicsPipelines;
  VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
  VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  module_info.codeSize = sizeof(k_present_vert);
  module_info.pCode = k_present_vert;
  bool ok = create_module(device, &module_info, nullptr, &vertex) == VK_SUCCESS;
  module_info.codeSize = sizeof(k_present_frag);
  module_info.pCode = k_present_frag;
  ok = ok && create_module(device, &module_info, nullptr, &fragment) == VK_SUCCESS;
  if (ok) {
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[0].pName = stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkGraphicsPipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &input;
    pipeline_info.pInputAssemblyState = &assembly;
    pipeline_info.pViewportState = &viewport;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = p.layout;
    pipeline_info.renderPass = p.pass;
    ok = create_pipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &p.pipeline) == VK_SUCCESS &&
         p.pipeline != VK_NULL_HANDLE;
  }
  if (vertex != VK_NULL_HANDLE) destroy_module(device, vertex, nullptr);
  if (fragment != VK_NULL_HANDLE) destroy_module(device, fragment, nullptr);
  p.ready = ok;
  return ok;
}
}  // namespace

void PresentPass::Destroy() {
  ready = false;
  if (device == VK_NULL_HANDLE) return;
  DropImages();
  if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
  if (pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, pool, nullptr);
  if (layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, layout, nullptr);
  if (set_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
  if (sampler != VK_NULL_HANDLE) vkDestroySampler(device, sampler, nullptr);
  if (pass != VK_NULL_HANDLE) vkDestroyRenderPass(device, pass, nullptr);
  pipeline = VK_NULL_HANDLE;
  pool = VK_NULL_HANDLE;
  layout = VK_NULL_HANDLE;
  set_layout = VK_NULL_HANDLE;
  sampler = VK_NULL_HANDLE;
  pass = VK_NULL_HANDLE;
  device = VK_NULL_HANDLE;
}

const PresentPass::Image* PresentPass::ImageFor(VkImage image, VkFormat image_format, VkExtent2D extent) {
  for (const Image& known : images) {
    if (known.image == image) return &known;
  }
  if (!ready || image_format != format || !extent.width || !extent.height) return nullptr;
  Image made;
  made.image = image;
  VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  // (the driver's own entry point: the layer's is for the game's textures)
  const auto create_view = layer.create_image_view ? layer.create_image_view : vkCreateImageView;
  if (create_view(device, &view_info, nullptr, &made.view) != VK_SUCCESS) return nullptr;
  VkFramebufferCreateInfo framebuffer_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  framebuffer_info.renderPass = pass;
  framebuffer_info.attachmentCount = 1;
  framebuffer_info.pAttachments = &made.view;
  framebuffer_info.width = extent.width;
  framebuffer_info.height = extent.height;
  framebuffer_info.layers = 1;
  if (vkCreateFramebuffer(device, &framebuffer_info, nullptr, &made.framebuffer) != VK_SUCCESS) {
    vkDestroyImageView(device, made.view, nullptr);
    return nullptr;
  }
  images.push_back(made);
  return &images.back();
}

namespace {

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
bool PlumeArmDriver() { return layer.arm.load(std::memory_order_relaxed); }
std::string PlumeDriverText() {
  std::lock_guard lock(layer.driver_text_mutex);
  return layer.driver_text;
}
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
    // How frames reach the screen (renut_plume_present_*). Without the pass the frame is blitted as before, and
    // a blit cannot turn it: no pre-rotation then.
    if (REXCVAR_GET(renut_plume_present_pass)) {
      const VkFormat present_format =
          s.format == RenderFormat::R8G8B8A8_UNORM ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
      if (!CreatePresentPass(s.pass, device->vk, present_format)) {
        REXLOG_WARN("Renut: Plume present pass not made; frames are blitted to the swapchain");
        s.pass.Destroy();
      }
    }
    s.samples = s.pass.ready;
    s.fit = REXCVAR_GET(renut_plume_present_fit);
    s.prerotate = s.pass.ready && REXCVAR_GET(renut_plume_present_prerotate);
    REXLOG_INFO("Renut: Plume present: {}, swapchain {}, {}",
                s.pass.ready ? "one render pass" : "clear and blit",
                s.fit ? "fitted to the frame" : "at the surface's size",
                s.prerotate ? "turned as the surface is" : "turned by the compositor");
    s.surface_w = uint32_t(std::max(ANativeWindow_getWidth(window), 0));
    s.surface_h = uint32_t(std::max(ANativeWindow_getHeight(window), 0));
    s.swap = s.queue->createSwapChain(RenderSwapChainDesc(ToRenderWindow(window), s.format, 3));
    if (s.swap) s.swap->setVsyncEnabled(REXCVAR_GET(renut_plume_vsync));
    Require(s.swap && renderer->ResizeSwapchain() && s.swap->getTextureCount(), "Plume: swapchain inválida.");
    for (auto& slot : s.slots) {
      slot.commands = s.queue->createCommandList();
      slot.fence = s.device->createCommandFence();
      slot.acquire = s.device->createCommandSemaphore();
      Require(slot.commands && slot.fence && slot.acquire,
              "Plume: falha ao alocar a apresentação.");
    }
    while (s.release.size() < s.swap->getTextureCount()) {
      s.release.push_back(s.device->createCommandSemaphore());
    }
    REXLOG_INFO("Renut: Plume renderer started on {} ({}x{})", s.gpu, s.swap->getWidth(),
                s.swap->getHeight());
    renderer->LogSurface("start");
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
  FollowSurfaceSize();
  auto& slot = s.slots[0];
  uint32_t index = 0;
  if (!s.swap->acquireTexture(slot.acquire.get(), &index)) {
    const bool resized = ResizeSwapchain();
    REXLOG_WARN("Renut: Plume acquire failed before the first frame; swapchain resize {} ({}x{})",
                resized ? "ok" : "FAILED", s.swap->getWidth(), s.swap->getHeight());
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

bool PlumeRenderer::PresentSamples() const { return state_->samples; }

// Every swapchain is made here (under swap_mutex): the layer is told what to make of Plume's request, the
// views and framebuffers of the old images go, and what was made is read back.
bool PlumeRenderer::ResizeSwapchain() {
  auto& s = *state_;
  if (!s.swap) return false;
  // What the frames in flight draw into goes with the old swapchain.
  WaitForAllSlots();
  s.pass.DropImages();
  // A frame was turned for a swapchain the pass can no longer draw into: the compositor turns them again.
  if (!s.pass.ready) s.prerotate = false;
  SwapchainPlan& plan = swapchain_plan;
  const bool planned = (s.fit || s.prerotate) && s.surface_w && s.surface_h;
  plan.physical = static_cast<VulkanDevice*>(s.device.get())->physicalDevice;
  plan.surface_w = planned ? s.surface_w : 0;
  plan.surface_h = planned ? s.surface_h : 0;
  plan.frame_w = s.fit ? s.fit_w : 0;
  plan.frame_h = s.fit ? s.fit_h : 0;
  plan.prerotate = s.prerotate;
  plan.extent = {};
  // Plume sizes its request from the window, which answers the size its buffers were last given (the fitted
  // or turned one): it is given the surface's own again, so that what Plume calls the swapchain's size stays
  // the surface's (FollowSurfaceSize compares the two).
  if (planned && s.window &&
      (uint32_t(ANativeWindow_getWidth(s.window)) != s.surface_w ||
       uint32_t(ANativeWindow_getHeight(s.window)) != s.surface_h)) {
    ANativeWindow_setBuffersGeometry(s.window, int32_t(s.surface_w), int32_t(s.surface_h), 0);
  }
  const bool resized = s.swap->resize();
  while (resized && s.release.size() < s.swap->getTextureCount()) {
    s.release.push_back(s.device->createCommandSemaphore());
  }
  if (resized && plan.extent.width && plan.extent.height) {
    s.extent = plan.extent;
    s.transform = plan.transform;
    s.surface_transform = plan.current;
  } else if (resized) {
    s.extent = {s.swap->getWidth(), s.swap->getHeight()};
    s.transform = s.surface_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  }
  s.blit_rect[0] = -1;
  if (resized) {
    REXLOG_INFO("Renut: Plume swapchain images {}x{}, pre-transform {} (surface {}x{}, fitted to {}x{})",
                s.extent.width, s.extent.height, TransformName(s.transform), s.surface_w, s.surface_h,
                plan.frame_w, plan.frame_h);
  }
  return resized;
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
      if (ResizeSwapchain()) {
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
  if (s.swap) FollowSurfaceSize();
  if (s.swap && s.fit) {
    // The swapchain is fitted to the frame's size; a frame of another size (the internal resolution option)
    // is followed once it has been shown for half a second or so, not at the first one.
    if (source && width && height && (width != s.fit_w || height != s.fit_h)) {
      if (width != s.fit_next_w || height != s.fit_next_h) {
        s.fit_next_w = width;
        s.fit_next_h = height;
        s.fit_count = 0;
      }
      if (++s.fit_count >= 30) {
        REXLOG_INFO("Renut: Plume frames are {}x{} now (the swapchain was fitted to {}x{}): recreating it", width,
                    height, s.fit_w, s.fit_h);
        s.fit_w = width;
        s.fit_h = height;
        s.fit_count = 0;
        ResizeSwapchain();
      }
    } else {
      s.fit_count = 0;
    }
  }
  // The pass was lost (a framebuffer the driver would not make) under a turned swapchain: a blit cannot turn.
  if (s.swap && !s.pass.ready && s.transform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ResizeSwapchain();
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
    if (s.swap && (!s.window || ANativeWindow_getWidth(s.window) <= 0 || ANativeWindow_getHeight(s.window) <= 0)) {
      // The surface is gone (Android answers a negative size): Plume would clamp that to the
      // largest extent and fail, leaving the swapchain's size at 4096x4096. The new surface
      // comes through AttachWindow.
      REXLOG_WARN("Renut: Plume acquire failed (frame {}) on a dead surface ({}x{}); no resize", s.frames.load(),
                  s.window ? ANativeWindow_getWidth(s.window) : -1, s.window ? ANativeWindow_getHeight(s.window) : -1);
    } else if (s.swap) {
      const bool resized = ResizeSwapchain();
      REXLOG_WARN("Renut: Plume acquire failed (frame {}); swapchain resize {} ({}x{}, {} images)",
                  s.frames.load(), resized ? "ok" : "FAILED", s.swap->getWidth(), s.swap->getHeight(),
                  s.swap->getTextureCount());
      LogSurface("after a failed acquire");
    } else {
      REXLOG_WARN("Renut: Plume present with no swapchain (frame {})", s.frames.load());
    }
    return;
  }
  auto* target = s.swap->getTexture(index);
  slot.commands->begin();
  VkCommandBuffer cb = static_cast<VulkanCommandList*>(slot.commands.get())->vk;
  VkImage dst = static_cast<VulkanTexture*>(target)->vk;
  // Black around a letterboxed frame; the game's last clear colour otherwise.
  VkClearColorValue background{};
  if (!source) {
    std::lock_guard lock(s.color_mutex);
    for (int i = 0; i < 4; ++i) background.float32[i] = s.clear_color[i];
  }
  // The images as they were made (the layer may have fitted and turned them), and the picture as the player
  // sees it: a quarter turn swaps its sides.
  const VkExtent2D extent =
      s.extent.width && s.extent.height ? s.extent : VkExtent2D{s.swap->getWidth(), s.swap->getHeight()};
  const bool sideways = s.transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
                        s.transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
  // Letterbox: keep the guest's aspect ratio inside the picture.
  const float sw = float(sideways ? extent.height : extent.width);
  const float sh = float(sideways ? extent.width : extent.height);
  int32_t x = 0, y = 0, w = 0, h = 0;
  if (source && width && height) {
    const float scale = std::min(sw / float(width), sh / float(height));
    w = int32_t(float(width) * scale), h = int32_t(float(height) * scale);
    x = (int32_t(sw) - w) / 2, y = (int32_t(sh) - h) / 2;
    if (s.blit_rect[0] != x || s.blit_rect[1] != y || s.blit_rect[2] != w || s.blit_rect[3] != h) {
      REXLOG_INFO("Renut: Plume shows the {}x{} frame at {}x{}+{}+{} of the {}x{} picture (images {}x{}, {}; "
                  "frame {})",
                  width, height, w, h, x, y, uint32_t(sw), uint32_t(sh), extent.width, extent.height,
                  TransformName(s.transform), s.frames.load());
      s.blit_rect[0] = x;
      s.blit_rect[1] = y;
      s.blit_rect[2] = w;
      s.blit_rect[3] = h;
    }
  }
  const auto* chain = static_cast<VulkanSwapChain*>(s.swap.get());
  const PresentPass::Image* image =
      s.pass.ready ? s.pass.ImageFor(dst, chain->pickedSurfaceFormat.format, extent) : nullptr;
  if (s.pass.ready && !image) {
    REXLOG_ERROR("Renut: Plume present pass: no framebuffer for a {}x{} swapchain image (format {}, pass {}); "
                 "frames are blitted from now on",
                 extent.width, extent.height, int(chain->pickedSurfaceFormat.format), int(s.pass.format));
    s.pass.ready = false;
  }
  if (image) {
    // One pass: the image is cleared as the pass begins (in the tiles, not by a fill of its own) and the frame
    // is a rectangle sampled from its texture, which the caller left in SHADER_READ.
    auto* texture = static_cast<VulkanTexture*>(source);
    const bool draw = texture && texture->imageView != VK_NULL_HANDLE && w > 0 && h > 0;
    const VkDescriptorSet set = s.pass.sets[slot_index % kPlumeFramesInFlight];
    if (draw) {
      VkDescriptorImageInfo sampled{s.pass.sampler, texture->imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      write.dstSet = set;
      write.dstBinding = 0;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      write.pImageInfo = &sampled;
      vkUpdateDescriptorSets(s.pass.device, 1, &write, 0, nullptr);
    }
    VkClearValue clear{};
    clear.color = background;
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = s.pass.pass;
    begin.framebuffer = image->framebuffer;
    begin.renderArea = {{0, 0}, extent};
    begin.clearValueCount = 1;
    begin.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &begin, VK_SUBPASS_CONTENTS_INLINE);
    if (draw) {
      const float texture_w = float(std::max<uint32_t>(uint32_t(texture->desc.width), 1u));
      const float texture_h = float(std::max<uint32_t>(uint32_t(texture->desc.height), 1u));
      PresentConstants constants{};
      // The picture is turned against the turn the compositor leaves out: a point (x, y) of it, y down, goes to
      // (-y, x) under "rotate 90", (-x, -y) under 180 and (y, -x) under 270. By columns.
      switch (s.transform) {
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
          constants.turn[1] = 1.f, constants.turn[2] = -1.f;
          break;
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
          constants.turn[0] = -1.f, constants.turn[3] = -1.f;
          break;
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
          constants.turn[1] = -1.f, constants.turn[2] = 1.f;
          break;
        default:
          constants.turn[0] = 1.f, constants.turn[3] = 1.f;
          break;
      }
      constants.extent[0] = float(w) / sw;
      constants.extent[1] = float(h) / sh;
      constants.uv_scale[0] = float(width) / texture_w;
      constants.uv_scale[1] = float(height) / texture_h;
      constants.uv_limit[0] = (float(width) - 0.5f) / texture_w;
      constants.uv_limit[1] = (float(height) - 0.5f) / texture_h;
      const VkViewport viewport{0.f, 0.f, float(extent.width), float(extent.height), 0.f, 1.f};
      const VkRect2D scissor{{0, 0}, extent};
      vkCmdSetViewport(cb, 0, 1, &viewport);
      vkCmdSetScissor(cb, 0, 1, &scissor);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pass.pipeline);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pass.layout, 0, 1, &set, 0, nullptr);
      vkCmdPushConstants(cb, s.pass.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(constants), &constants);
      vkCmdDraw(cb, 4, 1, 0, 0);
    }
    vkCmdEndRenderPass(cb);
    // The pass left the image ready to present; Plume, whose barriers go by what it last set, is told.
    static_cast<VulkanTexture*>(target)->textureLayout = RenderTextureLayout::PRESENT;
  } else {
    slot.commands->barriers(RenderBarrierStage::COPY,
                            RenderTextureBarrier(target, RenderTextureLayout::COPY_DEST));
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &background, 1, &range);
    if (source && w > 0 && h > 0) {
      // The caller left the texture to be sampled (PresentSamples) and the pass was lost since: the blit takes
      // it as a copy source and gives it back.
      if (s.samples) {
        slot.commands->barriers(RenderBarrierStage::COPY,
                                RenderTextureBarrier(source, RenderTextureLayout::COPY_SOURCE));
      }
      VkImageBlit blit{};
      blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      blit.srcOffsets[1] = {int32_t(width), int32_t(height), 1};
      blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      blit.dstOffsets[0] = {x, y, 0};
      blit.dstOffsets[1] = {x + w, y + h, 1};
      vkCmdBlitImage(cb, static_cast<VulkanTexture*>(source)->vk,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &blit, VK_FILTER_LINEAR);
      if (s.samples) {
        slot.commands->barriers(RenderBarrierStage::GRAPHICS,
                                RenderTextureBarrier(source, RenderTextureLayout::SHADER_READ));
      }
    }
    slot.commands->barriers(RenderBarrierStage::NONE,
                            RenderTextureBarrier(target, RenderTextureLayout::PRESENT));
  }
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
    s.pass.DropImages();
    s.swap.reset();
    if (s.window) ReleaseWindow(s.window);
    s.window = window;
    // The new surface's size is the window's until Java reports one for it (FollowSurfaceSize).
    s.surface_w = uint32_t(std::max(ANativeWindow_getWidth(window), 0));
    s.surface_h = uint32_t(std::max(ANativeWindow_getHeight(window), 0));
    s.swap = s.queue->createSwapChain(RenderSwapChainDesc(ToRenderWindow(window), s.format, 3));
    if (s.swap) s.swap->setVsyncEnabled(REXCVAR_GET(renut_plume_vsync));
    Require(s.swap && ResizeSwapchain() && s.swap->getTextureCount(), "Plume: swapchain inválida.");
    REXLOG_INFO("Renut: Plume swapchain recreated ({}x{})", s.swap->getWidth(), s.swap->getHeight());
    // A new surface: whatever Java said about the old one no longer counts, and the next
    // size it reports for this one is taken again even if it equals an earlier one.
    s.size_serial = 0;
    s.blit_rect[0] = -1;
    LogSurface("new surface");
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

void PlumeRenderer::SetSurfaceSize(PlumeWindow* window, uint32_t width, uint32_t height) {
  uint64_t serial;
  {
    std::lock_guard lock(surface_size.mutex);
    surface_size.window = window;
    surface_size.width = width;
    surface_size.height = height;
    serial = ++surface_size.serial;
  }
  REXLOG_INFO("Renut: surface {} reports {}x{} ({}; window says {}x{}, report {})", static_cast<const void*>(window),
              width, height, width >= height ? "landscape" : "portrait",
              window ? ANativeWindow_getWidth(window) : -1, window ? ANativeWindow_getHeight(window) : -1, serial);
}

void PlumeRenderer::LogSurface(const char* when) {
  auto& s = *state_;
  if (!s.swap) {
    REXLOG_INFO("Renut: Plume surface ({}): no swapchain", when);
    return;
  }
  auto* chain = static_cast<VulkanSwapChain*>(s.swap.get());
  auto* device = static_cast<VulkanDevice*>(s.device.get());
  VkSurfaceCapabilitiesKHR caps{};
  const VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device->physicalDevice, chain->surface, &caps);
  REXLOG_INFO("Renut: Plume surface ({}): window {}x{} format {}; swapchain {}x{}, {} images, present mode {}",
              when, s.window ? ANativeWindow_getWidth(s.window) : -1, s.window ? ANativeWindow_getHeight(s.window) : -1,
              s.window ? ANativeWindow_getFormat(s.window) : -1, s.swap->getWidth(), s.swap->getHeight(),
              s.swap->getTextureCount(), int(chain->createdPresentMode));
  if (result != VK_SUCCESS) {
    REXLOG_WARN("Renut: Plume surface ({}): vkGetPhysicalDeviceSurfaceCapabilitiesKHR = {}", when, int(result));
    return;
  }
  // The extent is the window's own shape (the Mali reports 1920x1200 under "rotate 90" on a
  // tablet held in landscape): the compositor turns the images after when preTransform is identity, and the
  // images are made turned when it is the surface's (renut_plume_present_prerotate, ResizeSwapchain).
  REXLOG_INFO("Renut: Plume surface ({}): currentExtent {}x{}, extent {}x{}..{}x{}, transform {} "
              "(supported {:#x}), images {}..{}",
              when, caps.currentExtent.width, caps.currentExtent.height, caps.minImageExtent.width, caps.minImageExtent.height, caps.maxImageExtent.width,
              caps.maxImageExtent.height, TransformName(caps.currentTransform), uint32_t(caps.supportedTransforms),
              caps.minImageCount, caps.maxImageCount);
}

void PlumeRenderer::FollowSurfaceSize() {
  auto& s = *state_;
  if (!s.swap) return;
  const PlumeWindow* window;
  uint32_t width, height;
  uint64_t serial;
  {
    std::lock_guard lock(surface_size.mutex);
    window = surface_size.window;
    width = surface_size.width;
    height = surface_size.height;
    serial = surface_size.serial;
  }
  if (serial != s.size_serial) {
    s.size_serial = serial;
    s.presents_since_check = 0;
    if (window != s.window) {
      REXLOG_INFO("Renut: Plume ignores a size for surface {} (it presents to {})", static_cast<const void*>(window),
                  static_cast<const void*>(s.window));
    } else if (!width || !height) {
      REXLOG_WARN("Renut: Plume ignores an empty surface size {}x{}", width, height);
    } else if (width == s.swap->getWidth() && height == s.swap->getHeight() &&
               (!(s.fit || s.prerotate) || (width == s.surface_w && height == s.surface_h))) {
      s.surface_w = width;
      s.surface_h = height;
      REXLOG_INFO("Renut: Plume swapchain already {}x{}, as the surface (frame {})", width, height, s.frames.load());
    } else {
      REXLOG_WARN("Renut: Plume swapchain is {}x{} but the surface is now {}x{} (frame {}): recreating it",
                  s.swap->getWidth(), s.swap->getHeight(), width, height, s.frames.load());
      // Images the presentation engine may still hold go first.
      WaitForAllSlots();
      // Plume sizes the swapchain from the window, which keeps answering the size its
      // buffers were last given: give it the surface's own.
      const int32_t window_w = ANativeWindow_getWidth(s.window), window_h = ANativeWindow_getHeight(s.window);
      if (uint32_t(window_w) != width || uint32_t(window_h) != height) {
        const int32_t set = ANativeWindow_setBuffersGeometry(s.window, int32_t(width), int32_t(height), 0);
        REXLOG_INFO("Renut: Plume window said {}x{}; buffers set to {}x{} (result {}, now {}x{})", window_w, window_h,
                    width, height, set, ANativeWindow_getWidth(s.window), ANativeWindow_getHeight(s.window));
      }
      s.surface_w = width;
      s.surface_h = height;
      const bool resized = ResizeSwapchain();
      if (resized && s.swap->getWidth() == width && s.swap->getHeight() == height) {
        REXLOG_INFO("Renut: Plume swapchain recreated at {}x{} ({} images)", s.swap->getWidth(), s.swap->getHeight(),
                    s.swap->getTextureCount());
      } else {
        REXLOG_ERROR("Renut: Plume swapchain resize to {}x{} {}: it is {}x{}", width, height,
                     resized ? "gave another size" : "FAILED", s.swap->getWidth(), s.swap->getHeight());
      }
      s.blit_rect[0] = -1;
      LogSurface("after following the surface");
    }
    return;
  }
  // Now and then, ask the surface itself too and log once each size that disagrees with
  // the swapchain, in case a change ever arrives without Java seeing it.
  if (++s.presents_since_check < 120) return;
  s.presents_since_check = 0;
  auto* chain = static_cast<VulkanSwapChain*>(s.swap.get());
  VkSurfaceCapabilitiesKHR caps{};
  if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(static_cast<VulkanDevice*>(s.device.get())->physicalDevice,
                                                chain->surface, &caps) != VK_SUCCESS ||
      caps.currentExtent.width == UINT32_MAX) {
    return;
  }
  // The phone was turned upside down: the surface keeps its size, so Java reports nothing, and only its
  // transform says it. The frames are turned the new way from the next swapchain on (until then the compositor
  // makes up the difference).
  if (s.prerotate && caps.currentTransform != s.surface_transform) {
    REXLOG_INFO("Renut: Plume surface transform is {} now (the swapchain was made under {}): recreating it",
                TransformName(caps.currentTransform), TransformName(s.surface_transform));
    ResizeSwapchain();
    // (whatever was made, this transform has been answered: one try each time it changes)
    s.surface_transform = caps.currentTransform;
    return;
  }
  // (a fitted or turned swapchain makes the window answer its images' size)
  if (caps.currentExtent.width == s.extent.width && caps.currentExtent.height == s.extent.height) return;
  const uint32_t sw = s.swap->getWidth(), sh = s.swap->getHeight();
  if (caps.currentExtent.width == sw && caps.currentExtent.height == sh) {
    s.logged_extent_w = s.logged_extent_h = 0;
  } else if (caps.currentExtent.width != s.logged_extent_w || caps.currentExtent.height != s.logged_extent_h) {
    s.logged_extent_w = caps.currentExtent.width;
    s.logged_extent_h = caps.currentExtent.height;
    REXLOG_WARN("Renut: Plume surface extent {}x{} (transform {}) differs from the {}x{} swapchain (frame {})",
                caps.currentExtent.width, caps.currentExtent.height, TransformName(caps.currentTransform), sw, sh,
                s.frames.load());
  }
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
    {
      std::lock_guard lock(layer.driver_text_mutex);
      layer.driver_text = fmt::format("{}\n{} {}\nVulkan {}.{}.{}", properties.properties.deviceName, driver.driverName,
                                      driver.driverInfo, VK_API_VERSION_MAJOR(properties.properties.apiVersion),
                                      VK_API_VERSION_MINOR(properties.properties.apiVersion),
                                      VK_API_VERSION_PATCH(properties.properties.apiVersion));
    }
    layer.qualcomm.store(driver.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY, std::memory_order_relaxed);
    layer.arm.store(driver.driverID == VK_DRIVER_ID_ARM_PROPRIETARY, std::memory_order_relaxed);
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

// Plume asks for a swapchain of the window's size that the compositor turns. What the renderer planned instead
// (SwapchainPlan): images of the frame's size in the surface's shape, which the compositor scales, and already
// turned the way the surface is, which leaves it nothing to turn.
VKAPI_ATTR VkResult VKAPI_CALL LayerCreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* info,
                                                    const VkAllocationCallbacks* allocator,
                                                    VkSwapchainKHR* swapchain) {
  SwapchainPlan& plan = swapchain_plan;
  VkSwapchainCreateInfoKHR edited = *info;
  VkSurfaceTransformFlagBitsKHR current = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  if (plan.surface_w && plan.surface_h) {
    uint32_t w = plan.surface_w, h = plan.surface_h;
    if (plan.frame_w && plan.frame_h) {
      // The frame is letterboxed into the surface's shape; the images are made smaller until it fills them
      // one texel to one, never larger than the surface.
      if (uint64_t(w) * plan.frame_h <= uint64_t(h) * plan.frame_w) {
        if (w > plan.frame_w) {
          h = uint32_t((uint64_t(h) * plan.frame_w + w / 2) / w);
          w = plan.frame_w;
        }
      } else if (h > plan.frame_h) {
        w = uint32_t((uint64_t(w) * plan.frame_h + h / 2) / h);
        h = plan.frame_h;
      }
    }
    VkSurfaceCapabilitiesKHR caps{};
    const bool known = plan.physical != VK_NULL_HANDLE &&
                       vkGetPhysicalDeviceSurfaceCapabilitiesKHR(plan.physical, info->surface, &caps) == VK_SUCCESS;
    VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    if (known) current = caps.currentTransform;
    if (plan.prerotate && known && (caps.supportedTransforms & caps.currentTransform)) {
      switch (caps.currentTransform) {
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
          std::swap(w, h);
          transform = caps.currentTransform;
          break;
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
          transform = caps.currentTransform;
          break;
        default:
          break;  // identity, or a mirrored one the pass does not draw: left to the compositor
      }
    }
    if (known) {
      w = std::clamp(w, caps.minImageExtent.width, std::max(caps.minImageExtent.width, caps.maxImageExtent.width));
      h = std::clamp(h, caps.minImageExtent.height, std::max(caps.minImageExtent.height, caps.maxImageExtent.height));
    }
    edited.imageExtent = {std::max(w, 1u), std::max(h, 1u)};
    edited.preTransform = transform;
  }
  VkResult result = layer.create_swapchain(device, &edited, allocator, swapchain);
  if (result != VK_SUCCESS && (edited.imageExtent.width != info->imageExtent.width ||
                               edited.imageExtent.height != info->imageExtent.height ||
                               edited.preTransform != info->preTransform)) {
    // The driver would not make it as planned: as Plume asked, then, which is what it always made.
    REXLOG_WARN("Renut: Plume swapchain {}x{} with pre-transform {} refused ({}); made as the window is instead",
                edited.imageExtent.width, edited.imageExtent.height, TransformName(edited.preTransform), int(result));
    edited = *info;
    result = layer.create_swapchain(device, &edited, allocator, swapchain);
  }
  if (result == VK_SUCCESS) {
    plan.extent = edited.imageExtent;
    plan.transform = edited.preTransform;
    plan.current = current;
    plan.format = edited.imageFormat;
  }
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
  if (vkCreateSwapchainKHR) {
    layer.create_swapchain = vkCreateSwapchainKHR;
    vkCreateSwapchainKHR = LayerCreateSwapchain;
  }
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
