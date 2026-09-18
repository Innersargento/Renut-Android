#include "plume_graphics.h"
#include "plume_vulkan.h"
#include <adrenotools/driver.h>
#include <rex/system/kernel_state.h>
#include <rex/graphics/xenos.h>
#include <rex/memory/utils.h>
#include <rex/logging.h>
#include <dlfcn.h>
#include <cstring>
#include <stdexcept>
#include <sstream>

namespace {
PFN_vkGetInstanceProcAddr entry = nullptr;
void* driver_handle = nullptr; // The isolated Android process owns the loader.
// Xenia / RexGlue texture/util.cpp, BSD-3-Clause, specialized for RGBA8.
constexpr uint32_t tiledOffset(uint32_t x, uint32_t y, uint32_t pitch) {
  pitch = (pitch + 31) & ~31u;
  uint32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << 9;
  uint32_t micro = ((x & 7) + ((y & 14) << 2)) << 2;
  uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 1) << 4);
  return ((offset & ~511u) << 3) + ((y & 16) << 7) + ((offset & 448) << 2) +
      (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 63);
}
static_assert(tiledOffset(0,0,64)==0 && tiledOffset(1,0,64)==4 && tiledOffset(4,0,64)==32);
static_assert(tiledOffset(0,1,64)==16 && tiledOffset(0,2,64)==256 && tiledOffset(0,16,64)==2048);
static_assert(tiledOffset(32,0,64)==4096 && tiledOffset(0,32,64)==8192);
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
}

VkResult renutInitializeVulkanLoader() {
  if (!entry) return VK_ERROR_INITIALIZATION_FAILED;
  volkInitializeCustom(entry);
  return vkCreateInstance ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
}

namespace renut {
using namespace plume;
using rex::X_STATUS;
namespace xenos = rex::graphics::xenos;
struct PlumeGraphics::State {
  ANativeWindow* window;
  std::string hooks, directory, library, gpu;
  const std::atomic<bool>& stopped;
  rex::memory::Memory* memory = nullptr;
  std::unique_ptr<VulkanInterface> api;
  std::unique_ptr<RenderDevice> device;
  std::unique_ptr<RenderCommandQueue> queue;
  std::unique_ptr<RenderSwapChain> swap;
  std::unique_ptr<RenderCommandList> commands;
  std::unique_ptr<RenderCommandFence> fence;
  std::unique_ptr<RenderCommandSemaphore> acquire;
  std::vector<std::unique_ptr<RenderCommandSemaphore>> release;
  std::unique_ptr<RenderBuffer> upload, readback;
  std::unique_ptr<RenderTexture> image;
  RenderFormat format = RenderFormat::UNKNOWN;
  uint32_t width=0, height=0;
  uint64_t frames=0, verified=0;
  State(ANativeWindow* w, std::string h, std::string d, std::string l, const std::atomic<bool>& stop)
      : window(w), hooks(std::move(h)), directory(std::move(d)), library(std::move(l)), stopped(stop) {
    ANativeWindow_acquire(window);
  }
  ~State() { ANativeWindow_release(window); }
};
PlumeGraphics::PlumeGraphics(ANativeWindow* w,std::string h,std::string d,std::string l,const std::atomic<bool>& stop)
    : state_(std::make_unique<State>(w,std::move(h),std::move(d),std::move(l),stop)) {}
PlumeGraphics::~PlumeGraphics() { Shutdown(); }
bool PlumeGraphics::has_presentation() const { return bool(state_->swap); }
X_STATUS PlumeGraphics::SetupPresentation(rex::ui::WindowedAppContext*) {
  auto& s=*state_;
  if(s.swap)return X_STATUS_SUCCESS;
  require(!s.stopped,"Apresentação cancelada.");
  require(!driver_handle,"O driver já foi inicializado; abra uma nova sessão.");
  // adrenotools concatenates directory + library name, so it needs the slash.
  std::string driver_dir=s.directory;
  if(!driver_dir.empty()&&driver_dir.back()!='/')driver_dir+='/';
  driver_handle=s.library.empty()?dlopen("libvulkan.so",RTLD_NOW|RTLD_LOCAL):
      adrenotools_open_libvulkan(RTLD_NOW|RTLD_LOCAL,ADRENOTOOLS_DRIVER_CUSTOM,nullptr,
          s.hooks.c_str(),driver_dir.c_str(),s.library.c_str(),nullptr,nullptr);
  require(driver_handle,"Falha ao carregar o driver Vulkan selecionado.");
  entry=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(driver_handle,"vkGetInstanceProcAddr"));
  require(entry,"Driver sem vkGetInstanceProcAddr.");
  s.api=std::make_unique<VulkanInterface>(); require(s.api->isValid(),"Instância Plume inválida.");
  s.device=s.api->createDevice(""); require(bool(s.device),"Dispositivo Plume indisponível.");
  s.gpu=s.device->getDescription().name;
  s.queue=s.device->createCommandQueue(RenderCommandListType::DIRECT);
  require(bool(s.queue),"Fila Plume indisponível.");
  auto* device=static_cast<VulkanDevice*>(s.device.get());
  VkAndroidSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};info.window=s.window;
  VkSurfaceKHR surface=VK_NULL_HANDLE;
  require(vkCreateAndroidSurfaceKHR(s.api->instance,&info,nullptr,&surface)==VK_SUCCESS,"Superfície indisponível.");
  uint32_t count=0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(device->physicalDevice,surface,&count,nullptr);
  std::vector<VkSurfaceFormatKHR> formats(count);
  auto result=vkGetPhysicalDeviceSurfaceFormatsKHR(device->physicalDevice,surface,&count,formats.data());
  vkDestroySurfaceKHR(s.api->instance,surface,nullptr);
  require(result==VK_SUCCESS,"Falha ao consultar formato de apresentação.");
  for(auto f:formats){
    if(f.format==VK_FORMAT_R8G8B8A8_UNORM){s.format=RenderFormat::R8G8B8A8_UNORM;break;}
    if(f.format==VK_FORMAT_B8G8R8A8_UNORM)s.format=RenderFormat::B8G8R8A8_UNORM;
  }
  require(s.format!=RenderFormat::UNKNOWN,"Superfície sem RGBA8/BGRA8.");
  s.swap=s.queue->createSwapChain(RenderSwapChainDesc(s.window,s.format,3));
  require(s.swap && s.swap->resize() && s.swap->getTextureCount(),"Swapchain Plume inválida.");
  s.width=s.swap->getWidth();s.height=s.swap->getHeight();
  require(s.width && s.height && uint64_t(s.width)*s.height<=4096*4096,"Superfície excede o limite.");
  s.commands=s.queue->createCommandList();s.fence=s.device->createCommandFence();s.acquire=s.device->createCommandSemaphore();
  for(uint32_t i=0;i<s.swap->getTextureCount();++i)s.release.push_back(s.device->createCommandSemaphore());
  const auto bytes=uint64_t(s.width)*s.height*4;
  s.upload=s.device->createBuffer(RenderBufferDesc::UploadBuffer(bytes));
  s.readback=s.device->createBuffer(RenderBufferDesc::ReadbackBuffer(bytes));
  s.image=s.device->createTexture(RenderTextureDesc::Texture2D(s.width,s.height,1,s.format));
  require(s.commands && s.fence && s.acquire && s.upload && s.readback && s.image,"Falha ao alocar apresentação.");
  return X_STATUS_SUCCESS;
}
X_STATUS PlumeGraphics::SetupGuestGpu(rex::runtime::FunctionDispatcher*,rex::system::KernelState* kernel) {
  state_->memory=kernel->memory();
  return SetupPresentation(nullptr);
}
void PlumeGraphics::InitializeRingBuffer(uint32_t,uint32_t) { throw std::runtime_error("Plume: tradução de comandos Xenos ainda não implementada."); }
void PlumeGraphics::EnableReadPointerWriteBack(uint32_t,uint32_t) { throw std::runtime_error("Plume: writeback Xenos ainda não implementado."); }
void PlumeGraphics::SetInterruptCallback(uint32_t,uint32_t) { throw std::runtime_error("Plume: interrupções Xenos ainda não implementadas."); }

bool PlumeGraphics::PresentFrontBuffer(const uint32_t* words,uint32_t width,uint32_t height) {
  if(!state_->frames)REXSYS_INFO("Plume: decoding first framebuffer");
  auto& s=*state_;require(s.memory && s.swap,"Backend Plume não inicializado.");
  require(!s.stopped,"Apresentação cancelada.");
  xenos::xe_gpu_texture_fetch_t fetch{};std::memcpy(&fetch,words,sizeof(fetch));
  require(width && height && width<=4096 && height<=4096,"Dimensões do framebuffer inválidas.");
  require(fetch.format==xenos::TextureFormat::k_8_8_8_8,"Plume: formato do framebuffer ainda não suportado.");
  require(fetch.dimension==xenos::DataDimension::k2DOrStacked && !fetch.stacked && !fetch.exp_adjust &&
      !fetch.num_format && uint32_t(fetch.sign_x)==0 && uint32_t(fetch.sign_y)==0 && uint32_t(fetch.sign_z)==0 && uint32_t(fetch.sign_w)==0,
      "Plume: interpretação do framebuffer ainda não suportada.");
  require(fetch.size_2d.width+1==width && fetch.size_2d.height+1==height,"Dimensões incoerentes com o fetch.");
  uint32_t pitch=fetch.pitch<<5;require(pitch>=width,"Pitch inválido.");
  uint64_t sourceBytes=fetch.tiled?uint64_t(pitch)*((height+31)&~31u)*4:uint64_t(pitch)*height*4;
  const uint32_t address=fetch.base_address<<12;
  require(uint64_t(address)+sourceBytes<=0x20000000,"Framebuffer fora da memória física.");
  auto* source=s.memory->TranslatePhysical(address);
  // Validate every mapped region before CPU reads guest memory.
  for(uint64_t offset=0;offset<sourceBytes;){
    size_t length=0;rex::memory::PageAccess access;
    require(rex::memory::QueryProtect(source+offset,length,access) && length &&
        (access==rex::memory::PageAccess::kReadOnly || access==rex::memory::PageAccess::kReadWrite),
        "Framebuffer aponta para memória não legível.");
    offset+=std::min<uint64_t>(length,sourceBytes-offset);
  }
  std::vector<uint8_t> pixels(uint64_t(s.width)*s.height*4,0);
  // Preserve aspect ratio, with a black border. Native shader integration can
  // later replace this bounded CPU conversion with a GPU texture pipeline.
  double scale=std::min(double(s.width)/width,double(s.height)/height);
  uint32_t dw=std::max(1u,uint32_t(width*scale)),dh=std::max(1u,uint32_t(height*scale));
  uint32_t left=(s.width-dw)/2,top=(s.height-dh)/2;
  for(size_t i=3;i<pixels.size();i+=4)pixels[i]=255;
  for(uint32_t y=0;y<dh;++y)for(uint32_t x=0;x<dw;++x){
    uint32_t sx=uint64_t(x)*width/dw,sy=uint64_t(y)*height/dh;
    uint64_t offset=fetch.tiled?tiledOffset(sx,sy,pitch):(uint64_t(sy)*pitch+sx)*4;
    require(offset+4<=sourceBytes,"Endereço tiled fora dos limites.");
    uint8_t components[6]={0,0,0,0,0,255};
    uint32_t endian=uint32_t(fetch.endianness);
    uint32_t xorMask=endian==1?1:endian==2?3:endian==3?2:0;
    for(unsigned c=0;c<4;++c)components[c]=source[offset+(c^xorMask)];
    auto* dst=pixels.data()+((uint64_t(y+top)*s.width)+x+left)*4;
    for(unsigned c=0;c<4;++c){unsigned component=(fetch.swizzle>>(3*c))&7;
      require(component<6,"Swizzle reservado não suportado.");
      dst[s.format==RenderFormat::B8G8R8A8_UNORM && c<3?2-c:c]=components[component];}
  }
  if(!s.frames)REXSYS_INFO("Plume: upload {}x{}",s.width,s.height);
  auto* mapped=s.upload->map();require(mapped,"Upload Vulkan não mapeável.");
  std::memcpy(mapped,pixels.data(),pixels.size());
  auto* vkUpload=static_cast<VulkanBuffer*>(s.upload.get());
  require(vmaFlushAllocation(static_cast<VulkanDevice*>(s.device.get())->allocator,vkUpload->allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,"Falha ao publicar upload.");
  s.upload->unmap();
  if(!s.frames)REXSYS_INFO("Plume: acquire");
  uint32_t index=0;require(s.swap->acquireTexture(s.acquire.get(),&index),"Superfície mudou; reabra o teste.");
  s.commands->begin();
  s.commands->barriers(RenderBarrierStage::COPY,RenderBufferBarrier(s.upload.get(),RenderBufferAccess::READ));
  s.commands->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(s.image.get(),RenderTextureLayout::COPY_DEST));
  s.commands->copyTextureRegion(RenderTextureCopyLocation::Subresource(s.image.get()),
      RenderTextureCopyLocation::PlacedFootprint(s.upload.get(),s.format,s.width,s.height,1,s.width));
  s.commands->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(s.image.get(),RenderTextureLayout::COPY_SOURCE));
  auto* target=s.swap->getTexture(index);
  s.commands->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(target,RenderTextureLayout::COPY_DEST));
  s.commands->copyTexture(target,s.image.get());
  bool verify=s.frames==0;
  if(verify){
    // The pinned Plume Vulkan backend implements buffer->image and image->image,
    // but not image->buffer in copyTextureRegion. Use Vulkan for diagnostic readback.
    VkBufferImageCopy region{};region.bufferRowLength=s.width;region.bufferImageHeight=s.height;
    region.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;region.imageSubresource.layerCount=1;
    region.imageExtent={s.width,s.height,1};
    vkCmdCopyImageToBuffer(static_cast<VulkanCommandList*>(s.commands.get())->vk,
        static_cast<VulkanTexture*>(s.image.get())->vk,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        static_cast<VulkanBuffer*>(s.readback.get())->vk,1,&region);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer=static_cast<VulkanBuffer*>(s.readback.get())->vk;barrier.size=VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(static_cast<VulkanCommandList*>(s.commands.get())->vk,
        VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&barrier,0,nullptr);
  }
  s.commands->barriers(RenderBarrierStage::NONE,RenderTextureBarrier(target,RenderTextureLayout::PRESENT));
  s.commands->end();
  const RenderCommandList* list=s.commands.get();auto* wait=s.acquire.get();auto* signal=s.release[index].get();
  if(!s.frames)REXSYS_INFO("Plume: submit");
  s.queue->executeCommandLists(&list,1,&wait,1,&signal,1,s.fence.get());
  if(!s.frames)REXSYS_INFO("Plume: present");
  bool presented=s.swap->present(index,&signal,1);
  if(!s.frames)REXSYS_INFO("Plume: waiting for GPU");
  auto* device=static_cast<VulkanDevice*>(s.device.get());auto fence=static_cast<VulkanCommandFence*>(s.fence.get())->vk;
  require(vkWaitForFences(device->vk,1,&fence,VK_TRUE,2000000000ULL)==VK_SUCCESS,"GPU excedeu o prazo de 2 segundos.");
  vkResetFences(device->vk,1,&fence);
  require(presented,"Falha ao apresentar o framebuffer.");
  if(verify){auto* bytes=s.readback->map();require(bytes,"Readback indisponível.");
    require(vmaInvalidateAllocation(device->allocator,static_cast<VulkanBuffer*>(s.readback.get())->allocation,0,VK_WHOLE_SIZE)==VK_SUCCESS,"Falha ao invalidar readback.");
    bool same=std::memcmp(bytes,pixels.data(),pixels.size())==0;s.readback->unmap();
    require(same,"Readback GPU divergente do framebuffer convertido.");++s.verified;}
  ++s.frames;return true;
}
void PlumeGraphics::Shutdown(){
  auto& s=*state_;if(s.device)vkDeviceWaitIdle(static_cast<VulkanDevice*>(s.device.get())->vk);
  s.image.reset();s.readback.reset();s.upload.reset();s.release.clear();s.acquire.reset();
  s.fence.reset();s.commands.reset();s.swap.reset();s.queue.reset();s.device.reset();s.api.reset();s.memory=nullptr;
}
std::string PlumeGraphics::report() const {
  const auto& s=*state_;std::ostringstream out;
  out<<"GPU: "<<s.gpu<<"\nDriver solicitado: "<<(s.library.empty()?"sistema":s.library)
      <<"\nQuadros VdSwap → Plume: "<<s.frames<<"\nReadbacks GPU conferidos: "<<s.verified;
  return out.str();
}
}
