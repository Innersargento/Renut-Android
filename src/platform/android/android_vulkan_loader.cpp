#include <adrenotools/driver.h>
#include <dlfcn.h>
#include <stdlib.h>

#include <mutex>
#include <stdexcept>
#include <string>

namespace {
std::mutex loader_mutex;
void* loader=nullptr; // Retained until the isolated runtime process exits.
}
void renutConfigureXenosDriver(const std::string& hooks,const std::string& directory,const std::string& library,bool one_ui_fix,
                               const std::string& tu_debug) {
  std::lock_guard lock(loader_mutex);
  if(loader)throw std::runtime_error("O driver já foi inicializado nesta sessão.");
  // Turnip reads its device-feature overrides when the driver initialises, so
  // this has to be in the environment before the library is opened.
  //
  // Without the hint, sampling a UBWC-compressed image the GPU wrote earlier
  // can return stale or undecoded data: with two frames in flight it showed up
  // as whole frames coming out black, and before that as every second frame
  // rendering blocky and dark. Same workaround the Winlator builds use on this
  // GPU. Never overwrites a value the session already set, so it stays
  // overridable from outside.
  if(one_ui_fix)setenv("FD_DEV_FEATURES","enable_tp_ubwc_flag_hint=1",0);
  // The selection in the app takes precedence over inherited debug flags.
  // An empty value explicitly disables the flags for this runtime process.
  setenv("TU_DEBUG",tu_debug.c_str(),1);
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
// What volk's dlopen("libvulkan.so") becomes inside Plume's translation unit
// (see the build file): the loader the session opened, with its driver, when
// there is one.
extern "C" void* renut_vulkan_dlopen(const char* name, int flags) {
  {
    std::lock_guard lock(loader_mutex);
    if (loader) return loader;
  }
  return dlopen(name, flags);
}
