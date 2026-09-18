#include <adrenotools/driver.h>
#include <dlfcn.h>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {
std::mutex loader_mutex;
void* loader=nullptr; // Retained until the isolated runtime process exits.
}
void renutConfigureXenosDriver(const std::string& hooks,const std::string& directory,const std::string& library) {
  std::lock_guard lock(loader_mutex);
  if(loader)throw std::runtime_error("O driver já foi inicializado nesta sessão.");
  // adrenotools concatenates directory + library name, so it needs the slash.
  std::string driver_dir=directory;
  if(!driver_dir.empty()&&driver_dir.back()!='/')driver_dir+='/';
  loader=library.empty()?dlopen("libvulkan.so",RTLD_NOW|RTLD_LOCAL):
      adrenotools_open_libvulkan(RTLD_NOW|RTLD_LOCAL,ADRENOTOOLS_DRIVER_CUSTOM,nullptr,
          hooks.c_str(),driver_dir.c_str(),library.c_str(),nullptr,nullptr);
  if(!loader)throw std::runtime_error("Falha ao carregar o driver Vulkan selecionado.");
}
void* renutXenosVulkanSymbol(const char* name) {
  std::lock_guard lock(loader_mutex);
  return loader?dlsym(loader,name):nullptr;
}
