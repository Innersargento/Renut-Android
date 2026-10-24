// rex::memory::QueryProtect on Linux and Android answers by reading all of
// /proc/self/maps (an ifstream, a scanf per line) and walking it for the one
// address. The write-watch fault handler (MMIOHandler::ExceptionCallback) calls
// it on every fault, only to ask whether another thread has already unwatched
// the page - a question the access violation callback right after it answers
// anyway from the heap's own watch and protection state, and recovers a page
// that is writable by then. Movie frames and the engine's texture rewrites
// fault thousands of times a second; that read was ~9% of all the game's CPU
// on a 15 s profile of Showdown Town (0.3 of 3.2 cores, on the guest threads).
//
// librexruntime reaches QueryProtect through its PLT, so a definition in this
// library, which is the root of the load group, answers for it. The fault
// handler's call gets the page reported as no-access, which makes it skip the
// early-out and go on to the callback; every other caller (Protect, asking for
// the old protection) gets the SDK's own answer.
#include <dlfcn.h>

#include <android/log.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/memory/utils.h>

namespace {

using QueryProtectFn = bool (*)(void*, size_t&, rex::memory::PageAccess&);

// The fault handler calls from one or two places; remember what each return
// address is so dladdr (a linear walk of the dynamic symbols) runs once per site.
struct Site {
  std::atomic<uintptr_t> address{0};
  std::atomic<bool> is_fault_handler{false};
};
constexpr int kSites = 8;
Site g_sites[kSites];
std::atomic<int> g_site_count{0};
std::atomic<uint64_t> g_fast_calls{0};

bool IsFaultHandler(uintptr_t return_address) {
  const int count = g_site_count.load(std::memory_order_acquire);
  for (int i = 0; i < count && i < kSites; ++i) {
    if (g_sites[i].address.load(std::memory_order_relaxed) == return_address) {
      return g_sites[i].is_fault_handler.load(std::memory_order_relaxed);
    }
  }
  Dl_info info{};
  const bool fast = dladdr(reinterpret_cast<void*>(return_address), &info) != 0 &&
                    info.dli_sname != nullptr && std::strstr(info.dli_sname, "MMIOHandler") != nullptr;
  const int slot = g_site_count.fetch_add(1, std::memory_order_acq_rel);
  if (slot < kSites) {
    g_sites[slot].is_fault_handler.store(fast, std::memory_order_relaxed);
    g_sites[slot].address.store(return_address, std::memory_order_release);
  }
  return fast;
}

QueryProtectFn SdkQueryProtect() {
  static QueryProtectFn fn = [] {
    // The handle's own scope starts at librexruntime itself, past this library's definition.
    void* handle = dlopen("librexruntime.so", RTLD_NOW | RTLD_NOLOAD);
    void* symbol =
        handle ? dlsym(handle, "_ZN3rex6memory12QueryProtectEPvRmRNS0_10PageAccessE") : nullptr;
    __android_log_print(ANDROID_LOG_INFO, "renut-qp", "SDK QueryProtect %p", symbol);
    return reinterpret_cast<QueryProtectFn>(symbol);
  }();
  return fn;
}

}  // namespace

namespace rex::memory {

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  if (IsFaultHandler(reinterpret_cast<uintptr_t>(__builtin_return_address(0)))) {
    const uint64_t n = g_fast_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n % 100000 == 0) {
      __android_log_print(ANDROID_LOG_INFO, "renut-qp", "write-watch fault fast path: %llu faults",
                          static_cast<unsigned long long>(n));
    }
    access_out = PageAccess::kNoAccess;
    length = page_size();
    return true;
  }
  QueryProtectFn sdk = SdkQueryProtect();
  if (!sdk) {
    access_out = PageAccess::kNoAccess;
    length = 0;
    return false;
  }
  return sdk(base_address, length, access_out);
}

}  // namespace rex::memory
