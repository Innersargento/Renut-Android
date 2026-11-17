#include "platform/guest.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>

#include <rex/gpu/graphics_system.h>
#include <rex/kernel/heap.h>
#include <rex/kernel/module.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/kernel/object/module_object.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/thread_registry.h>
#include <rex/kernel/thread_state.h>
#include <rex/kernel/xboxkrnl.h>
#include <rex/ppc/context.h>
#include <rex/ppc/recompiled_code.h>

#include "platform/log.h"

extern "C" void __imp__MmQueryAddressProtect(PPCContext& __restrict, uint8_t*);

namespace renut::host {

namespace {
std::atomic<rex::kernel::TitleRuntime*> g_title{nullptr};
std::atomic<uint8_t*> g_base{nullptr};
std::atomic<rex::IPhysicalWriteWatch*> g_watch{nullptr};

// The physical write watch of the title, as the SDK's title context gives one to a graphics driver: the guest
// address space's invalidation callbacks and page watches, whose access faults the address space handles
// itself.
class TitleWriteWatch final : public rex::IPhysicalWriteWatch {
 public:
  explicit TitleWriteWatch(rex::kernel::GuestAddressSpace& memory) : memory_(memory) {}

  rex::ScopeGuard AddWriteListener(rex::IPhysicalWriteListener& listener) override {
    void* registration = memory_.RegisterPhysicalMemoryInvalidationCallback(&WriteThunk, &listener);
    return rex::ScopeGuard(
        [&memory = memory_, registration] { memory.UnregisterPhysicalMemoryInvalidationCallback(registration); });
  }

  void WatchPhysicalWrites(rex::PhysicalRange range) override {
    if (uint64_t(range.address) + range.length > 0x20000000u) return;
    memory_.EnablePhysicalMemoryAccessCallbacks(range.address, range.length, true, false);
  }

  rex::ScopeGuard LockWriteWatches() override {
    return rex::ScopeGuard([lock = memory_.LockWatches()]() mutable { lock.unlock(); });
  }

 private:
  static std::pair<rex::u32, rex::u32> WriteThunk(void* context, rex::u32 start, rex::u32 length, bool exact) {
    const rex::PhysicalRange unwatch =
        static_cast<rex::IPhysicalWriteListener*>(context)->OnPhysicalWrite(rex::PhysicalRange{start, length}, exact);
    return {unwatch.address, unwatch.length};
  }

  rex::kernel::GuestAddressSpace& memory_;
};
std::unique_ptr<TitleWriteWatch> g_watch_owner;

// The guest address space, through the title's executable module (whose processor module keeps it).
rex::kernel::GuestAddressSpace* AddressSpaceOf(rex::kernel::TitleRuntime& title) {
  const auto executable = title.GetModuleRegistry().GetExecutable();
  if (!executable) return nullptr;
  auto module = title.GetObjectTable().Get<rex::kernel::ModuleObject>(*executable);
  if (!module || !module->processor_module.get()) return nullptr;
  return module->processor_module->GetAddressSpace();
}
}  // namespace

void Bind(rex::kernel::TitleRuntime& title) {
  g_base.store(title.GetGuestMemory().GetView().GetVirtualMembase());
  if (rex::kernel::GuestAddressSpace* memory = AddressSpaceOf(title)) {
    g_watch_owner = std::make_unique<TitleWriteWatch>(*memory);
    g_watch.store(g_watch_owner.get());
  } else {
    REXLOG_CAT_ERROR(rex::log::renut, "Renut: the title's executable has no address space; guest writes are not watched");
  }
  g_title.store(&title);
}

void Unbind() {
  g_title.store(nullptr);
  g_watch.store(nullptr);
  g_watch_owner.reset();
  g_base.store(nullptr);
}

rex::kernel::TitleRuntime* Title() { return g_title.load(); }

rex::Result<rex::ScopeGuard> StartHostThread(std::string_view name, rex::MoveOnlyFunction<void(std::stop_token)> fn) {
  rex::kernel::TitleRuntime* title = g_title.load();
  if (!title) return rex::Err<rex::ScopeGuard>(rex::ErrorCategory::kRuntime, "no title is bound");
  constexpr rex::u32 kStackSize = 128 * 1024;
  auto stop = std::make_shared<std::stop_source>();
  auto body = std::make_shared<rex::MoveOnlyFunction<void(std::stop_token)>>(std::move(fn));
  auto thread = std::make_unique<rex::kernel::KernelThread>(*title, kStackSize, 0,
                                                            [body, token = stop->get_token()]() {
                                                              (*body)(token);
                                                              return 0;
                                                            });
  thread->SetName(name);
  auto launched = title->GetThreadRegistry().Launch(std::move(thread));
  if (!launched) return std::unexpected(launched.error());
  const rex::kernel::X_HANDLE handle = *launched;
  return rex::ScopeGuard([title, stop, handle] {
    stop->request_stop();
    title->GetThreadRegistry().GetObjectWaiter().WaitGuestObject(handle, 0, 0, 0, nullptr);
    rex::kernel::CloseOwned(title->GetObjectTable(), handle);
  });
}

uint8_t* VirtualBase() { return g_base.load(); }


uint32_t SystemAlloc(uint32_t size, uint32_t alignment, bool physical) {
  rex::kernel::TitleRuntime* title = g_title.load();
  if (!title) return 0;
  auto allocated = title->GetGuestMemory().Allocate(size, alignment, rex::kernel::GuestProtect::kReadWrite,
                                                   physical ? rex::kernel::GuestHeap::kPhysical
                                                            : rex::kernel::GuestHeap::kSystem);
  return allocated ? *allocated : 0;
}

void SystemFree(uint32_t address) {
  if (rex::kernel::TitleRuntime* title = g_title.load()) (void)title->GetGuestMemory().Free(address);
}

uint64_t RunGuest(uint32_t address, const uint64_t* arguments, size_t count) {
  rex::kernel::ThreadState* thread = rex::kernel::ThreadState::Get();
  uint8_t* base = g_base.load();
  if (!thread || !base || !address) return 0;
  // A call of its own on the thread's guest stack, as the recompiled code makes one: a copy of the thread's
  // registers with the arguments in r3 to r10.
  PPCContext call = *thread->GetContext();
  call.r1.u32 -= 0x100;
  PPCRegister* registers[] = {&call.r3, &call.r4, &call.r5, &call.r6, &call.r7, &call.r8, &call.r9, &call.r10};
  for (size_t i = 0; i < count && i < 8; ++i) registers[i]->u64 = arguments[i];
  rex::ppc::ResolveFunction(address)(call, base);
  return call.r3.u64;
}

namespace {
// MmQueryAddressProtect's answer (X_PAGE_* flags, 0 for a page not committed) is writable as the heap's own
// range test counts it: read-write, or write-combined.
bool WritableFlags(uint32_t flags) {
  constexpr uint32_t kReadOnly = 0x02, kReadWrite = 0x04, kExecuteReadWrite = 0x40, kWriteCombine = 0x400;
  return (flags & (kReadWrite | kExecuteReadWrite)) || ((flags & kWriteCombine) && (flags & kReadOnly));
}
}  // namespace

bool PhysicalRangeWritable(uint32_t address, uint32_t length) {
  if (!length) return true;
  rex::kernel::ThreadState* thread = rex::kernel::ThreadState::Get();
  uint8_t* base = g_base.load();
  if (!thread || !thread->GetRuntime() || !base) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true))
      REXLOG_CAT_WARN(rex::log::renut, "Renut: page access asked off a kernel thread; the range counts as unwatchable");
    return false;
  }
  // The views of physical memory, the most common one first: the virtual address of physical p is
  // base + p - offset, and a page of the view's size is answered for as a whole.
  struct View {
    uint32_t base, page, offset;
  };
  static constexpr View kViews[] = {
      {0xA0000000u, 64u << 10, 0}, {0xE0000000u, 4u << 10, 0x1000}, {0xC0000000u, 16u << 20, 0}};
  PPCContext call{};
  uint64_t at = address & 0x1FFFFFFFu;
  const uint64_t end = at + length;
  if (end > 0x20000000u) return false;
  while (at < end) {
    uint64_t next = 0;
    for (const View& view : kViews) {
      if (at < view.offset) continue;
      const uint64_t in_view = at - view.offset;
      if (uint64_t(view.base) + in_view >= 0xFFD00000u) continue;
      call.r3.u64 = uint32_t(view.base + in_view);
      __imp__MmQueryAddressProtect(call, base);
      if (WritableFlags(call.r3.u32)) {
        next = (in_view / view.page + 1) * view.page + view.offset;
        break;
      }
    }
    if (!next) return false;
    at = next;
  }
  return true;
}

namespace {
std::mutex g_mode_mutex;
rex::VideoMode g_mode;
}  // namespace

void SetVideoMode(const rex::VideoMode& mode) {
  std::lock_guard lock(g_mode_mutex);
  g_mode = mode;
}

rex::VideoMode GetVideoMode() {
  std::lock_guard lock(g_mode_mutex);
  return g_mode;
}

void FillGuestVideoMode(uint32_t address) {
  rex::kernel::X_VIDEO_MODE mode;
  rex::kernel::FillXVideoMode(GetVideoMode(), mode);
  std::memcpy(Virtual(address), &mode, sizeof(mode));
}

uint32_t VideoFlags() {
  const rex::VideoMode mode = GetVideoMode();
  rex::kernel::X_VIDEO_MODE x;
  rex::kernel::FillXVideoMode(mode, x);
  return (uint32_t(x.is_widescreen) ? 1u : 0u) | (mode.width >= 1024 ? 2u : 0u) | (mode.width >= 1920 ? 4u : 0u);
}

namespace {
std::atomic<rex::gpu::IGraphicsSystem*> g_graphics{nullptr};
}  // namespace

void SetGraphics(rex::gpu::IGraphicsSystem* graphics) { g_graphics.store(graphics); }

void ReportFrame() {
  if (rex::gpu::IGraphicsSystem* graphics = g_graphics.load()) graphics->ReportFrame();
}

rex::IPhysicalWriteWatch* WriteWatch() { return g_watch.load(); }

}  // namespace renut::host
