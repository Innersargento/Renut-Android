// The aspect ratio option (renut_aspect_ratio): the game drawn at a wider screen than its 1280x720, with the
// world widened (Hor+: the height of the view is kept, the sides show more) and the interface kept at 16:9 in
// the middle. Ported from reNut's widescreen mode for the PC (src/renut_engine/gpu/guest/widescreen.cpp of
// masterspike52/reNut), which is written for the USA executable with title update 3. The addresses here are
// the same functions and data in this build's executable, found by what each one does: the comment above
// each names the PC address it stands for.

#include "guest/widescreen.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <string>

#include <android/native_window.h>

#include <rex/ppc/context.h>
#include <rex/ppc/hook.h>
#include <rex/ppc/recompiled_code.h>

#include "guest/guest_address.h"
#include "platform/android_drivers.h"
#include "platform/guest.h"
#include "platform/log.h"
#include "platform/settings.h"

namespace renut::widescreen {
namespace {

constexpr double kNativeAspect = 16.0 / 9.0;
// The widest the option goes; beyond it the interface and the screen pools have not been looked at.
constexpr double kMaxAspect = 21.0 / 9.0;

// The camera's projection aspect (sub_82239C40, PC sub_82208C50): the active camera's mode at +0x6C, where 3
// means the console's mode (0 = 4:3, 1 = 16:9, 2 = 2.35:1). PC 0x82E1B5F0 and 0x82E1B6B8.
constexpr uint32_t kActiveCameraVa = 0x82FADD20;
constexpr uint32_t kCameraAspectModeOffset = 0x6C;
constexpr uint32_t kConsoleAspectModeVa = 0x82FADDE8;
constexpr uint32_t kAspectModeConsole = 3;
constexpr uint32_t kAspectMode4x3 = 0;

// The screen pools (sub_823191B8, PC sub_822E8660): three of 1280x736 32bpp texels (0x398000 bytes) and one of
// 0x730000 bytes, made once (the flag at +0x380 of the block at 0x82F83268), with their recorded sizes. PC's
// block is at 0x82DF03C8; the offsets are the same.
constexpr uint32_t kPoolBlockVa = 0x82F83268;
constexpr uint32_t kPoolsCreatedVa = kPoolBlockVa + 0x380;
constexpr uint32_t kScreenPoolBytes = 0x398000;
constexpr uint32_t kImagePoolBytes = 0x730000;
constexpr uint32_t kScratchPoolSizeVa = kPoolBlockVa + 0x348;
constexpr uint32_t kScratchPoolVa = kPoolBlockVa + 0x34C;
constexpr uint32_t kCopyPoolSizeVa = kPoolBlockVa + 0x350;
constexpr uint32_t kDepthPoolSizeVa = kPoolBlockVa + 0x368;
// Scratch textures over the scratch pool, sized for 1280: the whole screen at its start, the split-screen
// halves at its end.
constexpr uint32_t kScreenScratchVa = kPoolBlockVa + 0x104;      // 1280x720
constexpr uint32_t kHalfWidthScratchVa = kPoolBlockVa + 0x138;   // 640x720
constexpr uint32_t kTopScratchVa = kPoolBlockVa + 0x16C;         // 1280x352
constexpr uint32_t kBottomScratchVa = kPoolBlockVa + 0x1A0;      // 1280x368
// A D3D base texture's fetch constant (six words), after the resource header and MipFlush.
constexpr uint32_t kTextureFetchOffset = 0x1C;

// sub_823C4BB0's depth-only quad over L.O.G.'s TV screen (PC sub_823912A8, 0x82DA2960): four clip-space vertices
// (x, y, z, pad) sized for 16:9, narrowed for a wider screen.
constexpr uint32_t kTvMaskVerticesVa = 0x82F39700;
constexpr uint32_t kTvMaskVertexCount = 4;
constexpr uint32_t kTvMaskVertexStride = 16;

std::atomic<uint32_t> g_screen_width{0};
std::atomic<bool> g_tv_mask_narrowed{false};
// sub_823191B8 is making its pools on this thread.
thread_local bool t_sizing_pools = false;
// sub_823E92E8 (XUI start) is sizing XUI's root on this thread, and the root.
thread_local bool t_sizing_xui_root = false;
std::atomic<uint32_t> g_xui_root{0};
// A marker (sub_825E2028, sub_825C5AF8: a world position projected to screen pixels, kept in the safe area) is
// placing its XUI element.
thread_local bool t_placing_marker = false;

uint8_t* Base() { return renut::host::VirtualBase(); }

uint32_t LoadU32(uint32_t va) {
  uint32_t value;
  std::memcpy(&value, renut::GuestPtr(Base(), va), sizeof(value));
  return __builtin_bswap32(value);
}

void StoreU32(uint32_t va, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(renut::GuestPtr(Base(), va), &value, sizeof(value));
}

float LoadF32(uint32_t va) { return std::bit_cast<float>(LoadU32(va)); }
void StoreF32(uint32_t va, float value) { StoreU32(va, std::bit_cast<uint32_t>(value)); }

constexpr uint32_t AlignUp(uint32_t value, uint32_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

// A 1-level tiled 32bpp 2D texture's size (XGSetTextureHeader): 32x32 tiles, 4 KB pages.
constexpr uint32_t Tiled32bppBytes(uint32_t width, uint32_t height) {
  return AlignUp(AlignUp(width, 32) * AlignUp(height, 32) * 4, 0x1000);
}

// The surface's shape in landscape, or 16:9 while there is none.
double SurfaceAspect() {
  ANativeWindow* window = renut::android::Surface();
  if (!window) return kNativeAspect;
  const int32_t w = ANativeWindow_getWidth(window), h = ANativeWindow_getHeight(window);
  if (w <= 0 || h <= 0) return kNativeAspect;
  return double(std::max(w, h)) / double(std::min(w, h));
}

double ConfiguredAspect() {
  const std::string setting = renut::GetSettings().aspect_ratio.Get();
  if (setting == "18:9") return 18.0 / 9.0;
  if (setting == "19.5:9") return 19.5 / 9.0;
  if (setting == "20:9") return 20.0 / 9.0;
  if (setting == "21:9") return 21.0 / 9.0;
  if (setting == "device") return SurfaceAspect();
  return kNativeAspect;
}

// A wider screen keeps the height; the width is a multiple of 16, so its halves and quarters (split screen,
// downsamples) stay whole.
uint32_t WidthForAspect(double aspect) {
  if (aspect <= kNativeAspect + 0.01) return kNativeWidth;
  const double width = double(kNativeHeight) * std::min(aspect, kMaxAspect);
  return std::max(uint32_t(std::lround(width / 16.0)) * 16, kNativeWidth);
}

bool Widened() { return ScreenWidth() > kNativeWidth; }

// Room for one screen of 32bpp texels, tiled (32-texel rows) or linear (64-texel rows): what each of the 1280x736
// pools holds.
uint32_t ScreenPoolBytes() {
  const uint32_t bytes = AlignUp(AlignUp(ScreenWidth(), 64) * AlignUp(kNativeHeight, 32) * 4, 0x1000);
  return std::max(bytes, kScreenPoolBytes);
}

// The texture header at `header_va`, as XGSetTextureHeader and XGOffsetResourceAddress make a 1-level 2D
// texture: pitch, size and base.
void SetTexture2D(uint32_t header_va, uint32_t width, uint32_t height, uint32_t base_address) {
  const uint32_t fetch = header_va + kTextureFetchOffset;
  StoreU32(fetch, (LoadU32(fetch) & ~(0x1FFu << 22)) | ((AlignUp(width, 32) >> 5) << 22));
  StoreU32(fetch + 4, (LoadU32(fetch + 4) & 0xFFFu) | (base_address & ~0xFFFu));
  StoreU32(fetch + 8, (LoadU32(fetch + 8) & ~0x3FFFFFFu) | ((height - 1) << 13) | (width - 1));
}

// After sub_823191B8 made its pools (at the sizes PoolAllocationBytes gave): their recorded sizes, and the
// scratch textures it sized for 1280.
void WidenScratchTextures() {
  const uint32_t width = ScreenWidth();
  const uint32_t pool_bytes = ScreenPoolBytes();
  StoreU32(kScratchPoolSizeVa, pool_bytes);
  StoreU32(kCopyPoolSizeVa, pool_bytes);
  StoreU32(kDepthPoolSizeVa, pool_bytes);
  const uint32_t pool = LoadU32(kScratchPoolVa);
  if (!pool) return;
  const uint32_t pool_end = pool + pool_bytes;
  SetTexture2D(kScreenScratchVa, width, kNativeHeight, pool);
  SetTexture2D(kHalfWidthScratchVa, width / 2, kNativeHeight, pool_end - Tiled32bppBytes(width / 2, kNativeHeight));
  SetTexture2D(kTopScratchVa, width, 352, pool_end - Tiled32bppBytes(width, 352));
  SetTexture2D(kBottomScratchVa, width, 368, pool_end - Tiled32bppBytes(width, 368));
  REXLOG_CAT_INFO(rex::log::renut, "Renut: widescreen: screen pools of {} bytes, scratch textures {} wide", pool_bytes, width);
}

// sub_823191B8's pool allocations, sized for the wider screen; the image pool only grows past its recorded size
// (a texture is placed by it).
uint32_t PoolAllocationBytes(uint32_t bytes) {
  if (bytes == kScreenPoolBytes) return ScreenPoolBytes();
  if (bytes == kImagePoolBytes) return bytes + ScreenPoolBytes() - kScreenPoolBytes;
  return bytes;
}

// Widens the projection aspect for `mode` by the screen's extra width (Hor+); 4:3 keeps its own.
void WidenAspect(PPCContext& ctx, uint32_t mode) {
  if (mode == kAspectMode4x3 || !Widened()) return;
  const float scale = float(ScreenWidth()) / float(kNativeWidth);
  ctx.f1.f64 = double(float(ctx.f1.f64) * scale);
}

uint32_t CameraAspectMode() {
  const uint32_t camera = LoadU32(kActiveCameraVa);
  const uint32_t mode = camera ? LoadU32(camera + kCameraAspectModeOffset) : kAspectModeConsole;
  return mode == kAspectModeConsole ? LoadU32(kConsoleAspectModeVa) : mode;
}

// The TV's quad narrowed as WidenAspect narrows the world on screen: once, as the width is kept until restart.
void NarrowTvMask() {
  if (!Widened() || g_tv_mask_narrowed.exchange(true)) return;
  const float scale = float(kNativeWidth) / float(ScreenWidth());
  for (uint32_t i = 0; i < kTvMaskVertexCount; ++i) {
    const uint32_t x = kTvMaskVerticesVa + i * kTvMaskVertexStride;
    StoreF32(x, LoadF32(x) * scale);
  }
}

// How far right XUI's 1280-wide space sits on the wider screen, in screen pixels: in the middle.
float XuiOffset() { return Widened() ? float(ScreenWidth() - kNativeWidth) * 0.5f : 0.0f; }

// Keeps XUI's view transform at the vertical scale on both axes and moves it right by XuiOffset, so its 1280x720
// layout is centred on the wider screen.
void CenterXuiView(uint32_t matrix_va) {
  if (!Widened() || !matrix_va) return;
  const float m0 = LoadF32(matrix_va), m5 = LoadF32(matrix_va + 20), m12 = LoadF32(matrix_va + 48);
  const float target_width = m0 * float(kNativeWidth);
  const float target_height = m5 * float(kNativeHeight);
  const uint32_t width = ScreenWidth();
  if (target_width > float(width) - 1.0f && target_width < float(AlignUp(width, 32)) + 1.0f &&
      std::fabs(target_height - float(kNativeHeight)) < 1.0f) {
    StoreF32(matrix_va, m5);
    StoreF32(matrix_va + 48, m12 + (float(width) - float(kNativeWidth) * m5) * 0.5f);
  }
}

}  // namespace

uint32_t ScreenWidth() {
  uint32_t width = g_screen_width.load(std::memory_order_acquire);
  if (width) return width;
  width = WidthForAspect(ConfiguredAspect());
  uint32_t expected = 0;
  if (g_screen_width.compare_exchange_strong(expected, width, std::memory_order_acq_rel)) {
    REXLOG_CAT_INFO(rex::log::renut, "Renut: widescreen: aspect ratio {} -> the game's screen is {}x{}",
                    renut::GetSettings().aspect_ratio.Get(), width, kNativeHeight);
    return width;
  }
  return expected;
}

}  // namespace renut::widescreen

using namespace renut::widescreen;

// void sub_823ED828(int query, int* mode, DWORD* width, DWORD* height, BOOL* hidef) (PC sub_823BB7C0): the
// screen size for the video mode (1280x720 for HD), widened here.
extern "C" void __imp__sub_823ED828(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_823ED828) {
  const uint32_t width_va = ctx.r5.u32, height_va = ctx.r6.u32;
  __imp__sub_823ED828(ctx, base);
  if (width_va && height_va && LoadU32(width_va) == kNativeWidth && LoadU32(height_va) == kNativeHeight)
    StoreU32(width_va, ScreenWidth());
}

// void sub_823191B8() (PC sub_822E8660): the screen pools and the textures over them, once.
extern "C" void __imp__sub_823191B8(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_823191B8) {
  const bool creates = LoadU32(kPoolsCreatedVa) == 0;
  t_sizing_pools = creates && Widened();
  __imp__sub_823191B8(ctx, base);
  if (t_sizing_pools) WidenScratchTextures();
  t_sizing_pools = false;
}

// LPVOID sub_82364C78(SIZE_T size, int alignment, DWORD protect) (PC sub_82333708): the game's XPhysicalAlloc.
extern "C" void __imp__sub_82364C78(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_82364C78) {
  if (t_sizing_pools) ctx.r3.u64 = PoolAllocationBytes(ctx.r3.u32);
  __imp__sub_82364C78(ctx, base);
}

// float sub_82239C40() (PC sub_82208C50): the active camera's projection aspect.
extern "C" void __imp__sub_82239C40(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_82239C40) {
  __imp__sub_82239C40(ctx, base);
  WidenAspect(ctx, CameraAspectMode());
}

// float sub_82236148() (PC sub_822050D0): the projection aspect for the console's mode.
extern "C" void __imp__sub_82236148(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_82236148) {
  __imp__sub_82236148(ctx, base);
  WidenAspect(ctx, LoadU32(kConsoleAspectModeVa));
}

// void sub_823C4BB0(this) (PC sub_823912A8): draws the TV's quad (kTvMaskVerticesVa).
extern "C" void __imp__sub_823C4BB0(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_823C4BB0) {
  NarrowTvMask();
  __imp__sub_823C4BB0(ctx, base);
}

// HRESULT XuiRenderSetViewTransform(HXUIDC, const D3DXMATRIX*) [0x82205C88] (PC 0x821D55E8).
extern "C" void __imp__sub_82205C88(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_82205C88) {
  CenterXuiView(ctx.r4.u32);
  __imp__sub_82205C88(ctx, base);
}

// int sub_823E92E8(app) (PC sub_823B7338): starts XUI; its root (app + 8) is sized to the back buffer with
// XuiElementSetBounds.
extern "C" void __imp__sub_823E92E8(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_823E92E8) {
  t_sizing_xui_root = true;
  __imp__sub_823E92E8(ctx, base);
  t_sizing_xui_root = false;
}

// HRESULT XuiElementSetBounds(HXUIOBJ, float width, float height) [0x829CA2A8] (PC 0x82A27180): XUI's root keeps
// the game's 1280 wide.
extern "C" void __imp__sub_829CA2A8(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_829CA2A8) {
  if (Widened()) {
    const uint32_t object = ctx.r3.u32;
    if (t_sizing_xui_root) g_xui_root = object;
    if (object && object == g_xui_root.load() && std::fabs(float(ctx.f1.f64) - float(ScreenWidth())) < 1.0f)
      ctx.f1.f64 = double(kNativeWidth);
  }
  __imp__sub_829CA2A8(ctx, base);
}

// A marker's screen position (screen pixels, from the screen's left) goes to its element in XUI's centred
// 1280-wide space. sub_825E2028 and sub_825C5AF8 (PC sub_825CA958 and sub_825AB958).
extern "C" void __imp__sub_825E2028(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_825E2028) {
  t_placing_marker = true;
  __imp__sub_825E2028(ctx, base);
  t_placing_marker = false;
}

extern "C" void __imp__sub_825C5AF8(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_825C5AF8) {
  t_placing_marker = true;
  __imp__sub_825C5AF8(ctx, base);
  t_placing_marker = false;
}

// HRESULT XuiElementSetPosition(HXUIOBJ, const D3DXVECTOR3*) [0x82267268] (PC 0x82235C88).
extern "C" void __imp__sub_82267268(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_82267268) {
  if (t_placing_marker && Widened() && ctx.r4.u32) StoreF32(ctx.r4.u32, LoadF32(ctx.r4.u32) - XuiOffset());
  __imp__sub_82267268(ctx, base);
}
