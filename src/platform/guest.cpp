#include "platform/guest.h"

#include <atomic>

#include <rex/kernel/thread_state.h>
#include <rex/ppc/context.h>
#include <rex/ppc/recompiled_code.h>

#include "platform/log.h"

extern "C" void __imp__MmQueryAddressProtect(PPCContext& __restrict, uint8_t*);

namespace renut::host {

namespace {
std::atomic<rex::kernel::TitleRuntime*> g_title{nullptr};
std::atomic<uint8_t*> g_base{nullptr};
std::atomic<rex::IPhysicalWriteWatch*> g_watch{nullptr};
}  // namespace

void Bind(rex::kernel::TitleRuntime& title) {
  g_base.store(title.GetGuestMemory().GetView().GetVirtualMembase());
  g_title.store(&title);
}

void Unbind() {
  g_title.store(nullptr);
  g_base.store(nullptr);
}

rex::kernel::TitleRuntime* Title() { return g_title.load(); }

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
std::atomic<uint32_t> g_mode_width{1280}, g_mode_height{720};
}  // namespace

void SetVideoMode(const rex::VideoMode& mode) {
  g_mode_width.store(mode.width);
  g_mode_height.store(mode.height);
}

rex::VideoMode GetVideoMode() {
  rex::VideoMode mode;
  mode.width = g_mode_width.load();
  mode.height = g_mode_height.load();
  return mode;
}

void SetWriteWatch(rex::IPhysicalWriteWatch* watch) { g_watch.store(watch); }
rex::IPhysicalWriteWatch* WriteWatch() { return g_watch.load(); }

}  // namespace renut::host
