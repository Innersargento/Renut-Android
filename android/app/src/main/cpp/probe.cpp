#include "plume_vulkan.h"
#include <adrenotools/driver.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <unistd.h>
#include <thread>
#include <jni.h>
#include <dlfcn.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <algorithm>

namespace {
std::atomic<bool> stopped{false};
PFN_vkGetInstanceProcAddr driverEntry = nullptr;
// One driver per isolated process. Keep its library loaded until process exit.
void* driverLibrary = nullptr;
std::string stringValue(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* p = env->GetStringUTFChars(value, nullptr);
    std::string out(p);
    env->ReleaseStringUTFChars(value, p);
    return out;
}
void captureErrors() {
    int fds[2];
    if (pipe(fds) != 0) return;
    dup2(fds[1], STDERR_FILENO); close(fds[1]);
    std::thread([fd=fds[0]] {
        FILE* input=fdopen(fd,"r"); if(!input){close(fd);return;}
        char line[2048];
        while(fgets(line,sizeof(line),input)) __android_log_write(ANDROID_LOG_WARN,"RenutVulkan",line);
        fclose(input);
    }).detach();
}
double percentile(std::vector<double> samples, double p) {
    if (samples.empty()) return 0;
    std::sort(samples.begin(), samples.end());
    return samples[std::min(samples.size()-1, size_t(std::ceil(p*samples.size())-1))];
}
}

VkResult renutInitializeVulkanLoader() {
    if (!driverEntry) return VK_ERROR_INITIALIZATION_FAILED;
    volkInitializeCustom(driverEntry);
    return vkCreateInstance ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
}

extern "C" JNIEXPORT void JNICALL
Java_org_renut_android_ProbeActivity_nativeStop(JNIEnv*, jclass) { stopped = true; }

extern "C" JNIEXPORT jstring JNICALL
Java_org_renut_android_ProbeActivity_nativeRun(JNIEnv* env, jclass, jobject surface,
        jstring hooks, jstring driverDir, jstring libraryName) {
    using namespace plume;
    using Clock = std::chrono::steady_clock;
    std::ostringstream report;
    report << "reNut 0.1 • teste de apresentação Plume\n"
           << "Carga: limpeza animada; não representa o jogo.\n";
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window) return env->NewStringUTF("Falha: superfície Android indisponível.");
    try {
        captureErrors();
        const auto hookPath = stringValue(env, hooks);
        const auto dir = stringValue(env, driverDir);
        const auto lib = stringValue(env, libraryName);
        if (!lib.empty()) {
            driverLibrary = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL,
                ADRENOTOOLS_DRIVER_CUSTOM, nullptr, hookPath.c_str(), dir.c_str(),
                lib.c_str(), nullptr, nullptr);
        } else driverLibrary = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!driverLibrary) throw std::runtime_error("Não foi possível carregar o driver Vulkan selecionado.");
        driverEntry = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(driverLibrary, "vkGetInstanceProcAddr"));
        if (!driverEntry) throw std::runtime_error("Driver sem vkGetInstanceProcAddr.");
        auto api = std::make_unique<VulkanInterface>();
        if (!api->isValid()) throw std::runtime_error("Plume não conseguiu criar a instância Vulkan.");
        auto device = api->createDevice("");
        if (!device) throw std::runtime_error("Plume não conseguiu criar o dispositivo gráfico.");
        auto* vkDevice = static_cast<VulkanDevice*>(device.get());
        const auto& info = device->getDescription();
        const auto& caps = device->getCapabilities();
        report << "GPU: " << info.name << "\nDriver version (raw): " << info.driverVersion
            << "\nOrigem solicitada: " << (lib.empty() ? "sistema" : lib)
            << "\nDescriptor indexing: " << caps.descriptorIndexing
            << " | Buffer device address: " << caps.bufferDeviceAddress << "\n";
        VkPhysicalDeviceDriverProperties driverProps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &driverProps;
        vkGetPhysicalDeviceProperties2(vkDevice->physicalDevice, &props);
        report << "Driver informado pela GPU: " << driverProps.driverName << " / " << driverProps.driverInfo << "\n";
        auto queue = device->createCommandQueue(RenderCommandListType::DIRECT);
        if (!queue) throw std::runtime_error("Fila gráfica indisponível.");
        VkAndroidSurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        surfaceInfo.window=window;
        VkSurfaceKHR testSurface=VK_NULL_HANDLE;
        if(vkCreateAndroidSurfaceKHR(api->instance,&surfaceInfo,nullptr,&testSurface)!=VK_SUCCESS)
            throw std::runtime_error("Não foi possível consultar os formatos da superfície Android.");
        uint32_t formatCount=0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(vkDevice->physicalDevice,testSurface,&formatCount,nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        VkResult formatResult=vkGetPhysicalDeviceSurfaceFormatsKHR(vkDevice->physicalDevice,testSurface,&formatCount,formats.data());
        vkDestroySurfaceKHR(api->instance,testSurface,nullptr);
        RenderFormat selected=RenderFormat::UNKNOWN;
        if(formatResult==VK_SUCCESS) for(const auto& format:formats) {
            if(format.format==VK_FORMAT_R8G8B8A8_UNORM) {selected=RenderFormat::R8G8B8A8_UNORM;break;}
            if(format.format==VK_FORMAT_B8G8R8A8_UNORM) selected=RenderFormat::B8G8R8A8_UNORM;
        }
        if(selected==RenderFormat::UNKNOWN) throw std::runtime_error("A superfície não oferece RGBA8/BGRA8 compatível.");
        report << "Formato: " << (selected==RenderFormat::R8G8B8A8_UNORM?"RGBA8":"BGRA8") << "\n";
        auto swap = queue->createSwapChain(RenderSwapChainDesc(window, selected, 3));
        if (!swap || static_cast<VulkanSwapChain*>(swap.get())->pickedSurfaceFormat.format==VK_FORMAT_UNDEFINED ||
                !swap->resize() || !swap->getTextureCount())
            throw std::runtime_error("Falha na apresentação Vulkan / swapchain.");
        auto commands = queue->createCommandList();
        auto fence = device->createCommandFence();
        auto acquire = device->createCommandSemaphore();
        std::vector<std::unique_ptr<RenderCommandSemaphore>> release;
        std::vector<std::unique_ptr<RenderFramebuffer>> buffers;
        for (uint32_t i = 0; i < swap->getTextureCount(); ++i) {
            const auto* texture = swap->getTexture(i);
            RenderFramebufferDesc desc;
            desc.colorAttachments = &texture;
            desc.colorAttachmentsCount = 1;
            buffers.push_back(device->createFramebuffer(desc));
            release.push_back(device->createCommandSemaphore());
        }
        uint32_t familiesCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(vkDevice->physicalDevice, &familiesCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familiesCount);
        vkGetPhysicalDeviceQueueFamilyProperties(vkDevice->physicalDevice, &familiesCount, families.data());
        const auto queueIndex = static_cast<VulkanCommandQueue*>(queue.get())->familyIndex;
        const uint32_t bits = families.at(queueIndex).timestampValidBits;
        report << "Timestamp valid bits: " << bits << " | Period (ns): " << props.properties.limits.timestampPeriod << "\n";
        VkQueryPool query = VK_NULL_HANDLE;
        VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = 2;
        if (bits && vkCreateQueryPool(vkDevice->vk, &queryInfo, nullptr, &query) != VK_SUCCESS)
            query = VK_NULL_HANDLE;
        std::vector<double> frameTimes, cpuTimes, gpuTimes;
        VkResult lastQueryResult = VK_NOT_READY;
        report << "Superfície: " << swap->getWidth() << " × " << swap->getHeight() << "\n";
        for (int frame = 0; frame < 600 && !stopped; ++frame) {
            auto start = Clock::now();
            uint32_t index;
            if (!swap->acquireTexture(acquire.get(), &index)) break;
            auto cpuStart = Clock::now();
            commands->begin();
            const auto cb = static_cast<VulkanCommandList*>(commands.get())->vk;
            if (query) {
                vkCmdResetQueryPool(cb, query, 0, 2);
                vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, query, 0);
            }
            auto* texture = swap->getTexture(index);
            commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(texture, RenderTextureLayout::COLOR_WRITE));
            commands->setFramebuffer(buffers[index].get());
            commands->setViewports(RenderViewport(0, 0, float(swap->getWidth()), float(swap->getHeight())));
            commands->setScissors(RenderRect(0, 0, swap->getWidth(), swap->getHeight()));
            commands->clearColor(0, RenderColor(0.025f, 0.05f, 0.065f, 1));
            const int w = int(swap->getWidth()), h = int(swap->getHeight());
            const int x = int((0.5 + 0.4 * std::sin(frame * 0.04)) * (w - w/5));
            RenderRect rect(x, h/3, x + w/5, h*2/3);
            commands->clearColor(0, RenderColor(0.35f, 0.9f, 0.65f, 1), &rect, 1);
            commands->barriers(RenderBarrierStage::NONE, RenderTextureBarrier(texture, RenderTextureLayout::PRESENT));
            if (query) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query, 1);
            commands->end();
            const RenderCommandList* list = commands.get();
            auto* wait = acquire.get();
            auto* signal = release[index].get();
            queue->executeCommandLists(&list, 1, &wait, 1, &signal, 1, fence.get());
            auto submitted = Clock::now();
            bool presented = swap->present(index, &signal, 1);
            auto vkFence=static_cast<VulkanCommandFence*>(fence.get())->vk;
            VkResult fenceResult=vkWaitForFences(vkDevice->vk,1,&vkFence,VK_TRUE,2000000000ULL);
            if(fenceResult!=VK_SUCCESS) throw std::runtime_error("Submissão GPU falhou ou excedeu 2 segundos: "+std::to_string(fenceResult));
            vkResetFences(vkDevice->vk,1,&vkFence);
            if (!presented) break;
            if (frame >= 30) {
                frameTimes.push_back(std::chrono::duration<double, std::milli>(Clock::now()-start).count());
                cpuTimes.push_back(std::chrono::duration<double, std::milli>(submitted-cpuStart).count());
                uint64_t values[2]{};
                if (query) lastQueryResult = vkGetQueryPoolResults(vkDevice->vk, query, 0, 2, sizeof(values), values,
                        sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
                if (query && lastQueryResult == VK_SUCCESS) {
                    const uint64_t mask = bits == 64 ? UINT64_MAX : ((uint64_t(1) << bits)-1);
                    gpuTimes.push_back(double((values[1]-values[0]) & mask) * props.properties.limits.timestampPeriod / 1e6);
                }
            }
        }
        vkDeviceWaitIdle(vkDevice->vk);
        if (query) vkDestroyQueryPool(vkDevice->vk, query, nullptr);
        report << "Amostras após 30 quadros de aquecimento: " << frameTimes.size() << "\n";
        if (frameTimes.empty()) report << "Teste interrompido ou apresentação falhou antes da medição.\n";
        else {
            report << "Quadro p50 / p95 / p99 (ms): " << percentile(frameTimes,.5) << " / "
                << percentile(frameTimes,.95) << " / " << percentile(frameTimes,.99)
                << "\nCPU preparação + submissão p50 (ms): " << percentile(cpuTimes,.5) << "\n";
            if (gpuTimes.empty()) report << "Timestamps GPU indisponíveis (query status: " << lastQueryResult << ").\n";
            else report << "GPU p50 / p95 (ms): " << percentile(gpuTimes,.5) << " / " << percentile(gpuTimes,.95) << "\n";
            report << "Teste " << (stopped ? "interrompido" : "finalizado") << ".\n";
        }
        report << "Execução serializada com fence por quadro; diagnóstico, não benchmark do Banjo.\n";
    } catch (const std::exception& error) { report << "Falha: " << error.what() << "\n"; }
    ANativeWindow_release(window);
    return env->NewStringUTF(report.str().c_str());
}
