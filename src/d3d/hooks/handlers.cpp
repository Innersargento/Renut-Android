// Layer 1a and 1b handlers: the state setters and the object-binding calls of the
// recompiled Direct3D, taken by the ported versions in state.cpp and bound.cpp.
//
// A handler returns true to take the call; the recompiled function then does not
// run (hooks/layer.cpp, Enter). Calls are still counted for the report and still
// pass through Leave, as every intercepted function does. The setters return
// nothing, so there is no result to leave in r3.
//
// The ports are written from reNut's title-update-3 code with the offsets translated to
// this release (device.h).
#include "d3d/device/bound.h"
#include "d3d/sync/fence.h"
#include "d3d/scene/frame.h"
#include "d3d/device/getters.h"
#include "d3d/device/leaves.h"
#include "d3d/resources/microcode.h"
#include "d3d/device/misc.h"
#include "d3d/resources/resource.h"
#include "d3d/resources/shader.h"
#include "d3d/scene/command_buffers.h"
#include "d3d/scene/swap.h"
#include "d3d/scene/tiling.h"
#include "d3d/device/internal.h"
#include "d3d/device/state.h"

#include "d3d/hooks/layer.h"
#include "d3d/frontend/native_d3d.h"
#include "guest/host_support.h"

#include <rex/cvar.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_BOOL(renut_native_fence_wait, true, "reNut",
                    "D3DDevice_IsFencePending, D3DDevice_BlockOnFence and CDevice::BlockOnFence natively: "
                    "the wait sleeps until the GPU backend completes a fence (the recompiled functions' "
                    "wait loop is not run). Off: the recompiled functions");

// The recompiled originals UnsetAll hands the shaders to.
extern "C" {
REX_FUNC(sub_82223270);  // D3D_SetSurfaceClip
REX_FUNC(sub_82210AB0);  // D3D_CDevice_KickOff
// Weak, so a build without them still links: the resource port checks before it calls.
__attribute__((weak)) void __imp__MmAllocatePhysicalMemoryEx(PPCContext& __restrict, uint8_t*);
__attribute__((weak)) void __imp__MmFreePhysicalMemory(PPCContext& __restrict, uint8_t*);
__attribute__((weak)) void sub_822399D8(PPCContext& __restrict, uint8_t*);  // the game's XMemFree
// What the device's creation and the ring call; weak, and the handlers decline when one is missing.
__attribute__((weak)) void sub_82265B90(PPCContext& __restrict, uint8_t*);  // the game's XMemAlloc
__attribute__((weak)) void sub_822402A8(PPCContext& __restrict, uint8_t*);  // the calling thread's id
__attribute__((weak)) void sub_82D0827C(PPCContext& __restrict, uint8_t*);  // the device type
__attribute__((weak)) void sub_82657160(PPCContext& __restrict, uint8_t*);
__attribute__((weak)) void sub_82663208(PPCContext& __restrict, uint8_t*);
__attribute__((weak)) void sub_826597C8(PPCContext& __restrict, uint8_t*);  // D3D::InitializeApiState
__attribute__((weak)) void sub_82659F30(PPCContext& __restrict, uint8_t*);  // D3D::InitializeHardwareDevice
__attribute__((weak)) void D3D__UninitializeDevice(PPCContext& __restrict, uint8_t*);  // 0x8265A198
__attribute__((weak)) void sub_822873E8(PPCContext& __restrict, uint8_t*);  // CDevice::StartNewSegment
__attribute__((weak)) void sub_8222CC50(PPCContext& __restrict, uint8_t*);  // D3DDevice_BeginVertices
REX_FUNC(sub_822084F0);  // D3D_DestroyResource (D3DResource_Destroy)
void __imp__sub_8222A0A8(PPCContext& __restrict, uint8_t*);  // D3DDevice_SetVertexShader
void __imp__sub_82226730(PPCContext& __restrict, uint8_t*);  // D3DDevice_SetPixelShader
}

namespace renut::d3d::port {
namespace {

// The call being served, for the handlers whose port ends in D3D::SetSurfaceClip
// (internal.h): the original runs on a copy of the caller's context.
struct ActiveCall {
  PPCContext* ctx = nullptr;
  uint8_t* base = nullptr;
};
thread_local ActiveCall active_call;

// Restores the call it found, so a handler that runs inside another one (a game call a
// handler makes that is itself ported) does not leave the outer one without its context.
struct CallScope {
  ActiveCall previous;
  CallScope(PPCContext& ctx, uint8_t* base) : previous(active_call) { active_call = {&ctx, base}; }
  ~CallScope() { active_call = previous; }
};

void RunSurfaceClip(Device device, i32 x1, i32 y1, i32 x2, i32 y2) {
  if (!active_call.ctx)
    return;
  PPCContext call = *active_call.ctx;
  call.r3.u32 = device.va;
  call.r4.u32 = u32(x1);
  call.r5.u32 = u32(y1);
  call.r6.u32 = u32(x2);
  call.r7.u32 = u32(y2);
  sub_82223270(call, active_call.base);  // the hooked D3D_SetSurfaceClip: the front end records it
}

// D3DDevice_SetRenderState_*(device, value)
template <u32 State>
bool RenderState(PPCContext& ctx, uint8_t* base) {
  if constexpr (State == 50) {  // ScissorTestEnable reapplies the scissor
    CallScope scope(ctx, base);
    SetRenderState(DeviceAt(base, ctx.r3.u32), State, ctx.r4.u32);
    return true;
  }
  SetRenderState(DeviceAt(base, ctx.r3.u32), State, ctx.r4.u32);
  return true;
}

// D3DDevice_SetSamplerState_*(device, sampler, value)
template <u32 State>
bool SamplerState(PPCContext& ctx, uint8_t* base) {
  SetSamplerState(DeviceAt(base, ctx.r3.u32), ctx.r4.u32, State, ctx.r5.u32);
  return true;
}

// D3DDevice_SetClipPlane(device, index, const float* plane)
bool ClipPlane(PPCContext& ctx, uint8_t* base) {
  const u32 plane = ctx.r5.u32;
  SetClipPlane(DeviceAt(base, ctx.r3.u32), ctx.r4.u32, plane ? renut::GuestPtr(base, plane) : nullptr);
  return true;
}

// D3DDevice_Set{Vertex,Pixel}ShaderConstant{B,I}(device, start, const* data, count)
template <u32 FirstWord, bool Integer>
bool ShaderConstants(PPCContext& ctx, uint8_t* base) {
  const u32 data = ctx.r5.u32;
  const u8* host = data ? renut::GuestPtr(base, data) : nullptr;
  if constexpr (Integer)
    SetShaderConstantsI(DeviceAt(base, ctx.r3.u32), FirstWord, ctx.r4.u32, host, ctx.r6.u32);
  else
    SetShaderConstantsB(DeviceAt(base, ctx.r3.u32), FirstWord, ctx.r4.u32, host, ctx.r6.u32);
  return true;
}

// D3DDevice_SetTexture(device, sampler, texture, PendingMask3), and its inline twin.
bool Texture(PPCContext& ctx, uint8_t* base) {
  SetTexture(DeviceAt(base, ctx.r3.u32), ctx.r4.u32, ctx.r5.u32);
  return true;
}

// D3DDevice_SetStreamSource(device, stream, buffer, offset, stride, PendingMask3)
bool StreamSource(PPCContext& ctx, uint8_t* base) {
  SetStreamSource(DeviceAt(base, ctx.r3.u32), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  return true;
}

bool Indices(PPCContext& ctx, uint8_t* base) {
  SetIndices(DeviceAt(base, ctx.r3.u32), ctx.r4.u32);
  return true;
}

bool VertexDeclaration(PPCContext& ctx, uint8_t* base) {
  SetVertexDeclaration(DeviceAt(base, ctx.r3.u32), ctx.r4.u32);
  return true;
}

bool Viewport(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  SetViewport(DeviceAt(base, ctx.r3.u32), ctx.r4.u32);
  return true;
}

// D3D::SetViewport(device, x, y, width, height, min z, max z) [0x82222F20], the floats in f1-f6.
bool ViewportFloats(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  SetViewportFloats(DeviceAt(base, ctx.r3.u32), float(ctx.f1.f64), float(ctx.f2.f64), float(ctx.f3.f64),
                    float(ctx.f4.f64), float(ctx.f5.f64), float(ctx.f6.f64));
  return true;
}

bool ScissorRect(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  SetScissorRectCall(DeviceAt(base, ctx.r3.u32), ctx.r4.u32);
  return true;
}

bool RenderTarget(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  SetRenderTarget(DeviceAt(base, ctx.r3.u32), ctx.r4.u32, ctx.r5.u32);
  return true;
}

bool DepthStencilSurface(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  SetDepthStencilSurface(DeviceAt(base, ctx.r3.u32), ctx.r4.u32);
  return true;
}

// The native front end records both predication calls itself (d3d/frontend, in
// OnEnter, ahead of this handler); these keep the device's side.
bool Predication(PPCContext& ctx, uint8_t* base) {
  SetPredication(DeviceAt(base, ctx.r3.u32), ctx.r4.u32);
  return true;
}

bool CommandBufferPredication(PPCContext& ctx, uint8_t* base) {
  SetCommandBufferPredication(DeviceAt(base, ctx.r3.u32), ctx.r4.u32, ctx.r5.u32);
  return true;
}

bool ShaderGPRAllocation(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  SetShaderGPRAllocation(DeviceAt(base, ctx.r3.u32), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  return true;
}

bool ScreenExtentQueryMode(PPCContext& ctx, uint8_t* base) {
  SetScreenExtentQueryMode(DeviceAt(base, ctx.r3.u32), ctx.r4.u32);
  return true;
}

// D3DDevice_FlushHiZStencil is D3D::InvalidateConstants (same address).
bool InvalidateConstantsCall(PPCContext& ctx, uint8_t* base) {
  InvalidateConstants(DeviceAt(base, ctx.r3.u32));
  return true;
}

// UnsetAll's shader unbinding stays the original's: SetVertexShader / SetPixelShader
// run on a copy of the caller's context, as if the game had called them.
struct OriginalShaders {
  PPCContext* ctx;
  uint8_t* base;
  u32 device;
};

void BindNoShader(void* user, bool vertex) {
  auto* original = static_cast<OriginalShaders*>(user);
  // The native shader setters; the original only when one declines (a device recording
  // a command buffer, whose list the recompiled code extends).
  const Device device = DeviceAt(original->base, original->device);
  if (vertex ? SetVertexShader(device, 0) : SetPixelShader(device, 0))
    return;
  PPCContext call = *original->ctx;
  call.r3.u32 = original->device;
  call.r4.u32 = 0;
  (vertex ? __imp__sub_8222A0A8 : __imp__sub_82226730)(call, original->base);
}

bool UnsetAllCall(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  OriginalShaders original{&ctx, base, ctx.r3.u32};
  ShaderBinder shaders;
  shaders.bind = BindNoShader;
  shaders.user = &original;
  UnsetAll(DeviceAt(base, ctx.r3.u32), shaders);
  return true;
}

// --- Layer 1c ---------------------------------------------------------------

bool ResourceAddRef(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = AddRefResource(GuestOnly(base), ctx.r3.u32);
  return true;
}

// D3DResource_Release: the last release hands the resource to the original
// D3DResource_Destroy, which frees its memory through the game's allocator.
struct DestroyCall {
  PPCContext* ctx;
  uint8_t* base;
};

void DestroyViaOriginal(void* user, u32 resource) {
  auto* call_info = static_cast<DestroyCall*>(user);
  PPCContext call = *call_info->ctx;
  call.r3.u32 = resource;
  sub_822084F0(call, call_info->base);
}

bool ResourceRelease(PPCContext& ctx, uint8_t* base) {
  DestroyCall call_info{&ctx, base};
  ResourceDestroyer destroyer;
  destroyer.destroy = DestroyViaOriginal;
  destroyer.user = &call_info;
  ctx.r3.u64 = ReleaseResource(GuestOnly(base), ctx.r3.u32, destroyer);
  return true;
}

bool ResourceGetType(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = ResourceType(GuestOnly(base), ctx.r3.u32);
  return true;
}

bool TextureLevelCount(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = BaseTextureLevelCount(GuestOnly(base), ctx.r3.u32);
  return true;
}

bool TextureArraySize(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = ArrayTextureSize(GuestOnly(base), ctx.r3.u32);
  return true;
}

bool RenderTargetGet(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = GetRenderTarget(DeviceAt(base, ctx.r3.u32), ctx.r4.u32);
  return true;
}

bool DepthStencilSurfaceGet(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = GetDepthStencilSurface(DeviceAt(base, ctx.r3.u32));
  return true;
}

bool StreamSourceGet(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = GetStreamSource(DeviceAt(base, ctx.r3.u32), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  return true;
}

bool ViewportGet(PPCContext& ctx, uint8_t* base) {
  GetViewport(DeviceAt(base, ctx.r3.u32), ctx.r4.u32);
  return true;
}

bool VertexFetchConstant(PPCContext& ctx, uint8_t* base) {
  SetVertexFetchConstant(DeviceAt(base, ctx.r3.u32), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  return true;
}

bool IndexedVerticesEnd(PPCContext& ctx, uint8_t* base) {
  EndIndexedVertices(DeviceAt(base, ctx.r3.u32));
  return true;
}


bool DisplayModeGet(PPCContext& ctx, uint8_t* base) {
  GetDisplayMode(DeviceAt(base, ctx.r3.u32), ctx.r5.u32);
  return true;
}


// --- Fences -------------------------------------------------------------

// A fence wait kicks through the original CDevice::KickOff, whose hook publishes
// the recorded list and which issues the fence (the ring stays the original's).
struct KickCall {
  PPCContext* ctx;
  uint8_t* base;
};

void KickOffDevice(void* user, u32 device_va) {
  auto* kick = static_cast<KickCall*>(user);
  PPCContext call = *kick->ctx;
  call.r3.u32 = device_va;
  sub_82210AB0(call, kick->base);
}

FenceHooks MakeFenceHooks(KickCall& kick) {
  FenceHooks hooks;
  hooks.kick = KickOffDevice;
  hooks.executor_idle = renut::native_d3d::ExecutorIdle;
  hooks.pause = renut::WaitIfTitlePaused;
  hooks.epoch = renut::native_d3d::FenceEpoch;
  hooks.wait = renut::native_d3d::FenceWait;
  hooks.user = &kick;
  return hooks;
}




// --- Resources ----------------------------------------------------------

// The recompiled functions resource memory goes through, run on a copy of the context
// of the call being served.
u32 AllocPhysicalCall(u32 size, u32 protect, u32 alignment) {
  if (!active_call.ctx || !__imp__MmAllocatePhysicalMemoryEx)
    return 0;
  PPCContext call = *active_call.ctx;
  call.r3.u32 = 0;
  call.r4.u32 = size;
  call.r5.u32 = protect;
  call.r6.u32 = 0;
  call.r7.u32 = 0xFFFFFFFFu;
  call.r8.u32 = alignment;
  __imp__MmAllocatePhysicalMemoryEx(call, active_call.base);
  return call.r3.u32;
}

void FreePhysicalCall(u32 address) {
  if (!active_call.ctx || !__imp__MmFreePhysicalMemory)
    return;
  PPCContext call = *active_call.ctx;
  call.r3.u32 = 0;
  call.r4.u32 = address;
  __imp__MmFreePhysicalMemory(call, active_call.base);
}

void XMemFreeCall(u32 address, u32 attributes) {
  if (!active_call.ctx || !sub_822399D8)
    return;
  PPCContext call = *active_call.ctx;
  call.r3.u32 = address;
  call.r4.u32 = attributes;
  sub_822399D8(call, active_call.base);
}

void WaitFenceCall(u32 fence) {
  if (!active_call.ctx)
    return;
  KickCall kick{active_call.ctx, active_call.base};
  BlockOnFence(DeviceAt(active_call.base, renut::native_d3d::MainDevice()), fence, false,
               MakeFenceHooks(kick));
}

// D3DDevice_IsFencePending(fence) (sub_8228F650): fence 0 is never pending; the current
// fence is kicked first, as it is only issued by a KickOff, then the test.
bool IsFencePendingCall(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(renut_native_fence_wait))
    return false;
  const u32 fence = ctx.r3.u32;
  if (!fence) {
    ctx.r3.u64 = 0;
    return true;
  }
  const Device device = DeviceAt(base, renut::native_d3d::MainDevice());
  if (!device) {
    ctx.r3.u64 = 0;
    return true;
  }
  if (fence == device.W(off::FenceTag)) {
    KickCall kick{&ctx, base};
    KickOffDevice(&kick, device.va);
  }
  ctx.r3.u64 = FencePending(device, fence) ? 1 : 0;
  return true;
}

// CDevice::BlockOnFence(device, fence, type, no kick) (sub_8228F558).
bool CDeviceBlockOnFenceCall(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(renut_native_fence_wait))
    return false;
  KickCall kick{&ctx, base};
  BlockOnFence(DeviceAt(base, ctx.r3.u32), ctx.r4.u32, (ctx.r6.u32 & 0xFF) != 0, MakeFenceHooks(kick));
  return true;
}

// D3DDevice_BlockOnFence(fence) (sub_8228F3B0): the title's device, kick allowed.
bool BlockOnFenceCall(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(renut_native_fence_wait))
    return false;
  if (ctx.r3.u32) {
    KickCall kick{&ctx, base};
    BlockOnFence(DeviceAt(base, renut::native_d3d::MainDevice()), ctx.r3.u32, false, MakeFenceHooks(kick));
  }
  return true;
}

bool CreateVertexBufferCall(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  ctx.r3.u64 = CreateVertexBuffer(ctx.r3.u32, ctx.r4.u32);
  return true;
}

bool CreateIndexBufferCall(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  ctx.r3.u64 = CreateIndexBuffer(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  return true;
}

// D3DDevice_CreateTexture(Width, Height, Depth, Levels, Usage, Format, Pool, Type)
bool CreateTextureCall(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  ctx.r3.u64 = CreateTexture(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                             ctx.r10.u32);
  return true;
}

bool CreateSurfaceCall(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  ctx.r3.u64 = CreateSurface(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  return true;
}

bool LockVertexBufferCall(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  ctx.r3.u64 = LockVertexBuffer(ctx.r3.u32, ctx.r6.u32);
  return true;
}

bool LockIndexBufferCall(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  ctx.r3.u64 = LockIndexBuffer(ctx.r3.u32, ctx.r6.u32);
  return true;
}

bool LockArrayTextureCall(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  LockArrayTextureRect(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
  return true;
}

bool LockLineTextureCall(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  LockLineTextureRect(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  return true;
}

bool LockSurfaceCall(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  LockSurfaceRect(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  return true;
}

bool LockVolumeCall(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  LockVolumeBox(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  return true;
}

bool UnlockVertexBufferCall(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  UnlockVertexBuffer(ctx.r3.u32);
  return true;
}

bool UnlockSurfaceCall(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  UnlockSurfaceRect(ctx.r3.u32);
  return true;
}

bool UnlockArrayTextureCall(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  UnlockArrayTextureRect(ctx.r3.u32);
  return true;
}

bool UnlockConstantBufferCall(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  UnlockConstantBuffer(ctx.r3.u32);
  return true;
}

bool DestroyResourceCall(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  DestroyResource(ctx.r3.u32);
  return true;
}

bool SurfaceDescCall(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  GetSurfaceDesc(ctx.r3.u32, ctx.r4.u32);
  return true;
}

bool VolumeDescCall(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  GetVolumeDesc(ctx.r3.u32, ctx.r4.u32);
  return true;
}

bool SurfaceLevelCall(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  ctx.r3.u64 = GetSurfaceLevel(ctx.r3.u32, ctx.r4.u32);
  return true;
}

bool CubeMapSurfaceCall(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  ctx.r3.u64 = GetCubeMapSurface(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  return true;
}

bool VolumeLevelCall(PPCContext& ctx, uint8_t* base) {
  CallScope scope(ctx, base);
  ctx.r3.u64 = GetVolumeLevel(ctx.r3.u32, ctx.r4.u32);
  return true;
}

// --- Calls into the recompiled game --------------------------------------

// A call into the recompiled game on a copy of the context of the call being served.
struct GameCall {
  PPCContext* ctx;
  uint8_t* base;
};

u32 RunGame(void* user, void (*function)(PPCContext&, uint8_t*), u32 r3, u32 r4 = 0) {
  auto* game = static_cast<GameCall*>(user);
  PPCContext call = *game->ctx;
  call.r3.u32 = r3;
  call.r4.u32 = r4;
  function(call, game->base);
  return call.r3.u32;
}














// --- Shaders -------------------------------------------------------------

u32 ShaderXMemAlloc(void* user, u32 size, u32 attributes) { return RunGame(user, sub_82265B90, size, attributes); }
void ShaderXMemFree(void* user, u32 address, u32 attributes) { RunGame(user, sub_822399D8, address, attributes); }

ShaderCalls MakeShaderCalls(GameCall& game) {
  ShaderCalls calls;
  calls.xmem_alloc = ShaderXMemAlloc;
  calls.xmem_free = ShaderXMemFree;
  calls.user = &game;
  return calls;
}

// D3DVertexShader_Bind(shader, unused, declaration, strides)
bool ShaderBind(PPCContext& ctx, uint8_t* base) {
  BindVertexShader(base, ctx.r3.u32, ctx.r5.u32, ctx.r6.u32);
  return true;
}

bool ShaderSetVertex(PPCContext& ctx, uint8_t* base) {
  return SetVertexShader(DeviceAt(base, ctx.r3.u32), ctx.r4.u32);
}

bool ShaderSetPixel(PPCContext& ctx, uint8_t* base) {
  return SetPixelShader(DeviceAt(base, ctx.r3.u32), ctx.r4.u32);
}

// XGRegisterVertexShader(shader, physical) / XGRegisterPixelShader(shader, physical)
bool ShaderRegisterVertex(PPCContext& ctx, uint8_t* base) {
  RegisterVertexShader(base, ctx.r3.u32, ctx.r4.u32);
  return true;
}

bool ShaderRegisterPixel(PPCContext& ctx, uint8_t* base) {
  RegisterPixelShader(base, ctx.r3.u32, ctx.r4.u32);
  return true;
}

bool ShaderCreateVertex(PPCContext& ctx, uint8_t* base) {
  if (!sub_82265B90 || !sub_822399D8)
    return false;
  GameCall game{&ctx, base};
  u32 result = 0;
  if (!CreateVertexShader(base, ctx.r3.u32, MakeShaderCalls(game), result))
    return false;
  ctx.r3.u64 = result;
  return true;
}

bool ShaderCreatePixel(PPCContext& ctx, uint8_t* base) {
  if (!sub_82265B90 || !sub_822399D8)
    return false;
  GameCall game{&ctx, base};
  u32 result = 0;
  if (!CreatePixelShader(base, ctx.r3.u32, MakeShaderCalls(game), result))
    return false;
  ctx.r3.u64 = result;
  return true;
}

bool ShaderCreateDeclaration(PPCContext& ctx, uint8_t* base) {
  if (!sub_82265B90 || !sub_822399D8)
    return false;
  GameCall game{&ctx, base};
  ctx.r3.u64 = CreateVertexDeclaration(base, ctx.r3.u32, MakeShaderCalls(game));
  return true;
}

// SetVertexShaderConstantFN(device, start, data, count, PendingMask)
bool ShaderConstantsVertexF(PPCContext& ctx, uint8_t* base) {
  return SetVertexShaderConstantsF(DeviceAt(base, ctx.r3.u32), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u64);
}

bool ShaderConstantsPixelF(PPCContext& ctx, uint8_t* base) {
  return SetPixelShaderConstantsF(DeviceAt(base, ctx.r3.u32), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u64);
}

// --- Smaller device calls ------------------------------------------------

bool MiscGetDeviceCaps(PPCContext& ctx, uint8_t* base) {
  GetDeviceCaps(base, ctx.r4.u32);
  return true;
}

bool MiscGetResourceDevice(PPCContext& ctx, uint8_t* base) {
  if (!sub_82D0827C)
    return false;
  GameCall game{&ctx, base};
  GetResourceDevice(base, RunGame(&game, sub_82D0827C, 0), ctx.r4.u32);
  return true;
}


// D3DDevice_DrawVerticesUP(device, primitive, vertex count, data, stride): room for
// the vertices from BeginVertices (the native front end's), then the data.
bool MiscDrawVerticesUP(PPCContext& ctx, uint8_t* base) {
  if (!sub_8222CC50)
    return false;
  const u32 device_va = ctx.r3.u32, count = ctx.r5.u32, data = ctx.r6.u32, stride = ctx.r7.u32;
  PPCContext call = ctx;
  call.r6.u32 = stride;
  sub_8222CC50(call, base);
  if (call.r3.u32)
    FinishVerticesUP(DeviceAt(base, device_va), call.r3.u32, data, count * stride);
  return true;
}

}  // namespace

namespace {

void RegisterMisc() {
  SetHandler(kD3DDevice_GetDeviceCaps, MiscGetDeviceCaps);
  SetHandler(kD3DResource_GetDevice, MiscGetResourceDevice);
  SetHandler(kD3DDevice_DrawVerticesUP, MiscDrawVerticesUP);
}

void RegisterShaders() {
  SetHandler(kD3DVertexShader_Bind, ShaderBind);
  SetHandler(kD3DDevice_SetVertexShader, ShaderSetVertex);
  SetHandler(kD3DDevice_SetPixelShader, ShaderSetPixel);
  SetHandler(kXGRegisterVertexShader, ShaderRegisterVertex);
  SetHandler(kXGRegisterPixelShader, ShaderRegisterPixel);
  SetHandler(kD3DDevice_CreateVertexShader, ShaderCreateVertex);
  SetHandler(kD3DDevice_CreatePixelShader, ShaderCreatePixel);
  SetHandler(kD3DDevice_CreateVertexDeclaration, ShaderCreateDeclaration);
  SetHandler(kD3DDevice_SetVertexShaderConstantFN, ShaderConstantsVertexF);
  SetHandler(kD3DDevice_SetPixelShaderConstantFN, ShaderConstantsPixelF);
}



void RegisterResources() {
  GuestCalls calls;
  calls.alloc_physical = AllocPhysicalCall;
  calls.free_physical = FreePhysicalCall;
  calls.xmem_free = XMemFreeCall;
  calls.wait_fence = WaitFenceCall;
  SetGuestCalls(calls);
  SetHandler(kD3DDevice_IsFencePending, IsFencePendingCall);
  SetHandler(kD3D_CDevice_BlockOnFence, CDeviceBlockOnFenceCall);
  SetHandler(kD3DDevice_BlockOnFence, BlockOnFenceCall);
  SetHandler(kD3DDevice_CreateVertexBuffer, CreateVertexBufferCall);
  SetHandler(kD3DDevice_CreateIndexBuffer, CreateIndexBufferCall);
  SetHandler(kD3DDevice_CreateTexture, CreateTextureCall);
  SetHandler(kD3DDevice_CreateSurface, CreateSurfaceCall);
  SetHandler(kD3DVertexBuffer_Lock, LockVertexBufferCall);
  SetHandler(kD3DIndexBuffer_Lock, LockIndexBufferCall);
  SetHandler(kD3DArrayTexture_LockRect, LockArrayTextureCall);
  SetHandler(kD3DLineTexture_LockRect, LockLineTextureCall);
  SetHandler(kD3DSurface_LockRect, LockSurfaceCall);
  SetHandler(kD3DVolume_LockBox, LockVolumeCall);
  SetHandler(kD3DVertexBuffer_Unlock, UnlockVertexBufferCall);
  SetHandler(kD3DSurface_UnlockRect, UnlockSurfaceCall);
  SetHandler(kD3DArrayTexture_UnlockRect, UnlockArrayTextureCall);
  SetHandler(kD3DConstantBuffer_Unlock, UnlockConstantBufferCall);
  SetHandler(kD3D_DestroyResource, DestroyResourceCall);
  SetHandler(kD3DSurface_GetDesc, SurfaceDescCall);
  SetHandler(kD3DVolume_GetDesc, VolumeDescCall);
  SetHandler(kD3DTexture_GetSurfaceLevel, SurfaceLevelCall);
  SetHandler(kD3DCubeTexture_GetCubeMapSurface, CubeMapSurfaceCall);
  SetHandler(kD3DVolumeTexture_GetVolumeLevel, VolumeLevelCall);
}


void RegisterLayer1c() {
  SetHandler(kD3DResource_AddRef, ResourceAddRef);
  SetHandler(kD3DResource_Release, ResourceRelease);
  SetHandler(kD3DResource_GetType, ResourceGetType);
  SetHandler(kD3DBaseTexture_GetLevelCount, TextureLevelCount);
  SetHandler(kD3DArrayTexture_GetArraySize, TextureArraySize);
  SetHandler(kD3DDevice_GetRenderTarget, RenderTargetGet);
  SetHandler(kD3DDevice_GetDepthStencilSurface, DepthStencilSurfaceGet);
  SetHandler(kD3DDevice_GetStreamSource, StreamSourceGet);
  SetHandler(kD3DDevice_GetViewport, ViewportGet);
  SetHandler(kD3DDevice_SetVertexFetchConstant, VertexFetchConstant);
  SetHandler(kD3DDevice_EndIndexedVertices, IndexedVerticesEnd);
  SetHandler(kD3DDevice_GetDisplayMode, DisplayModeGet);
}

void RegisterLayer1a() {
  // Render states, by their index in N&B's table (off_82E506F0).
  SetHandler(kD3DDevice_SetRenderState_ZEnable, RenderState<10>);
  SetHandler(kD3DDevice_SetRenderState_ZFunc, RenderState<11>);
  SetHandler(kD3DDevice_SetRenderState_ZWriteEnable, RenderState<12>);
  SetHandler(kD3DDevice_SetRenderState_FillMode, RenderState<13>);
  SetHandler(kD3DDevice_SetRenderState_CullMode, RenderState<14>);
  SetHandler(kD3DDevice_SetRenderState_AlphaBlendEnable, RenderState<15>);
  SetHandler(kD3DDevice_SetRenderState_SeparateAlphaBlendEnable, RenderState<16>);
  SetHandler(kD3DDevice_SetRenderState_BlendFactor, RenderState<17>);
  SetHandler(kD3DDevice_SetRenderState_SrcBlend, RenderState<18>);
  SetHandler(kD3DDevice_SetRenderState_DestBlend, RenderState<19>);
  SetHandler(kD3DDevice_SetRenderState_BlendOp, RenderState<20>);
  SetHandler(kD3DDevice_SetRenderState_SrcBlendAlpha, RenderState<21>);
  SetHandler(kD3DDevice_SetRenderState_DestBlendAlpha, RenderState<22>);
  SetHandler(kD3DDevice_SetRenderState_BlendOpAlpha, RenderState<23>);
  SetHandler(kD3DDevice_SetRenderState_AlphaTestEnable, RenderState<24>);
  SetHandler(kD3DDevice_SetRenderState_AlphaRef, RenderState<25>);
  SetHandler(kD3DDevice_SetRenderState_AlphaFunc, RenderState<26>);
  SetHandler(kD3DDevice_SetRenderState_StencilEnable, RenderState<27>);
  SetHandler(kD3DDevice_SetRenderState_TwoSidedStencilMode, RenderState<28>);
  SetHandler(kD3DDevice_SetRenderState_StencilFail, RenderState<29>);
  SetHandler(kD3DDevice_SetRenderState_StencilZFail, RenderState<30>);
  SetHandler(kD3DDevice_SetRenderState_StencilPass, RenderState<31>);
  SetHandler(kD3DDevice_SetRenderState_StencilFunc, RenderState<32>);
  SetHandler(kD3DDevice_SetRenderState_StencilRef, RenderState<33>);
  SetHandler(kD3DDevice_SetRenderState_StencilMask, RenderState<34>);
  SetHandler(kD3DDevice_SetRenderState_StencilWriteMask, RenderState<35>);
  SetHandler(kD3DDevice_SetRenderState_CCWStencilFail, RenderState<36>);
  SetHandler(kD3DDevice_SetRenderState_CCWStencilZFail, RenderState<37>);
  SetHandler(kD3DDevice_SetRenderState_CCWStencilPass, RenderState<38>);
  SetHandler(kD3DDevice_SetRenderState_CCWStencilFunc, RenderState<39>);
  SetHandler(kD3DDevice_SetRenderState_CCWStencilRef, RenderState<40>);
  SetHandler(kD3DDevice_SetRenderState_CCWStencilMask, RenderState<41>);
  SetHandler(kD3DDevice_SetRenderState_CCWStencilWriteMask, RenderState<42>);
  SetHandler(kD3DDevice_SetRenderState_ClipPlaneEnable, RenderState<43>);
  SetHandler(kD3DDevice_SetRenderState_PointSize, RenderState<44>);
  SetHandler(kD3DDevice_SetRenderState_PointSizeMin, RenderState<45>);
  SetHandler(kD3DDevice_SetRenderState_PointSpriteEnable, RenderState<46>);
  SetHandler(kD3DDevice_SetRenderState_PointSizeMax, RenderState<47>);
  SetHandler(kD3DDevice_SetRenderState_MultiSampleAntiAlias, RenderState<48>);
  SetHandler(kD3DDevice_SetRenderState_MultiSampleMask, RenderState<49>);
  SetHandler(kD3DDevice_SetRenderState_ScissorTestEnable, RenderState<50>);
  SetHandler(kD3DDevice_SetRenderState_SlopeScaleDepthBias, RenderState<51>);
  SetHandler(kD3DDevice_SetRenderState_DepthBias, RenderState<52>);
  SetHandler(kD3DDevice_SetRenderState_ColorWriteEnable, RenderState<53>);
  SetHandler(kD3DDevice_SetRenderState_ColorWriteEnable1, RenderState<54>);
  SetHandler(kD3DDevice_SetRenderState_ColorWriteEnable2, RenderState<55>);
  SetHandler(kD3DDevice_SetRenderState_ColorWriteEnable3, RenderState<56>);
  SetHandler(kD3DDevice_SetRenderState_TessellationMode, RenderState<57>);
  SetHandler(kD3DDevice_SetRenderState_MinTessellationLevel, RenderState<58>);
  SetHandler(kD3DDevice_SetRenderState_MaxTessellationLevel, RenderState<59>);
  SetHandler(kD3DDevice_SetRenderState_Wrap0, RenderState<60>);
  SetHandler(kD3DDevice_SetRenderState_Wrap1, RenderState<61>);
  SetHandler(kD3DDevice_SetRenderState_Wrap2, RenderState<62>);
  SetHandler(kD3DDevice_SetRenderState_Wrap3, RenderState<63>);
  SetHandler(kD3DDevice_SetRenderState_Wrap4, RenderState<64>);
  SetHandler(kD3DDevice_SetRenderState_Wrap5, RenderState<65>);
  SetHandler(kD3DDevice_SetRenderState_Wrap6, RenderState<66>);
  SetHandler(kD3DDevice_SetRenderState_Wrap7, RenderState<67>);
  SetHandler(kD3DDevice_SetRenderState_Wrap8, RenderState<68>);
  SetHandler(kD3DDevice_SetRenderState_Wrap9, RenderState<69>);
  SetHandler(kD3DDevice_SetRenderState_Wrap10, RenderState<70>);
  SetHandler(kD3DDevice_SetRenderState_Wrap11, RenderState<71>);
  SetHandler(kD3DDevice_SetRenderState_Wrap12, RenderState<72>);
  SetHandler(kD3DDevice_SetRenderState_Wrap13, RenderState<73>);
  SetHandler(kD3DDevice_SetRenderState_Wrap14, RenderState<74>);
  SetHandler(kD3DDevice_SetRenderState_Wrap15, RenderState<75>);
  SetHandler(kD3DDevice_SetRenderState_ViewportEnable, RenderState<76>);
  SetHandler(kD3DDevice_SetRenderState_HighPrecisionBlendEnable, RenderState<77>);
  SetHandler(kD3DDevice_SetRenderState_HighPrecisionBlendEnable1, RenderState<78>);
  SetHandler(kD3DDevice_SetRenderState_HighPrecisionBlendEnable2, RenderState<79>);
  SetHandler(kD3DDevice_SetRenderState_HighPrecisionBlendEnable3, RenderState<80>);
  SetHandler(kD3DDevice_SetRenderState_HalfPixelOffset, RenderState<81>);
  SetHandler(kD3DDevice_SetRenderState_PrimitiveResetEnable, RenderState<82>);
  SetHandler(kD3DDevice_SetRenderState_PrimitiveResetIndex, RenderState<83>);
  SetHandler(kD3DDevice_SetRenderState_AlphaToMaskEnable, RenderState<84>);
  SetHandler(kD3DDevice_SetRenderState_AlphaToMaskOffsets, RenderState<85>);
  SetHandler(kD3DDevice_SetRenderState_GuardBand_X, RenderState<86>);
  SetHandler(kD3DDevice_SetRenderState_GuardBand_Y, RenderState<87>);
  SetHandler(kD3DDevice_SetRenderState_DiscardBand_X, RenderState<88>);
  SetHandler(kD3DDevice_SetRenderState_DiscardBand_Y, RenderState<89>);
  SetHandler(kD3DDevice_SetRenderState_HiStencilEnable, RenderState<90>);
  SetHandler(kD3DDevice_SetRenderState_HiStencilWriteEnable, RenderState<91>);
  SetHandler(kD3DDevice_SetRenderState_HiStencilFunc, RenderState<92>);
  SetHandler(kD3DDevice_SetRenderState_HiStencilRef, RenderState<93>);
  SetHandler(kD3DDevice_SetRenderState_PresentInterval, RenderState<94>);
  SetHandler(kD3DDevice_SetRenderState_PresentImmediateThreshold, RenderState<95>);
  SetHandler(kD3DDevice_SetRenderState_HiZEnable, RenderState<96>);
  SetHandler(kD3DDevice_SetRenderState_HiZWriteEnable, RenderState<97>);
  SetHandler(kD3DDevice_SetRenderState_LastPixel, RenderState<98>);
  SetHandler(kD3DDevice_SetRenderState_LineWidth, RenderState<99>);
  SetHandler(kD3DDevice_SetRenderState_Buffer2Frames, RenderState<100>);

  // Sampler states, by their index in N&B's table (off_82E50BD0).
  SetHandler(kD3DDevice_SetSamplerState_AddressU, SamplerState<0>);
  SetHandler(kD3DDevice_SetSamplerState_AddressV, SamplerState<1>);
  SetHandler(kD3DDevice_SetSamplerState_AddressW, SamplerState<2>);
  SetHandler(kD3DDevice_SetSamplerState_BorderColor, SamplerState<3>);
  SetHandler(kD3DDevice_SetSamplerState_MagFilter, SamplerState<4>);
  SetHandler(kD3DDevice_SetSamplerState_MinFilter, SamplerState<5>);
  SetHandler(kD3DDevice_SetSamplerState_MipFilter, SamplerState<6>);
  SetHandler(kD3DDevice_SetSamplerState_MipMapLodBias, SamplerState<7>);
  SetHandler(kD3DDevice_SetSamplerState_MaxMipLevel, SamplerState<8>);
  SetHandler(kD3DDevice_SetSamplerState_MaxAnisotropy, SamplerState<9>);
  SetHandler(kD3DDevice_SetSamplerState_MagFilterZ, SamplerState<10>);
  SetHandler(kD3DDevice_SetSamplerState_MinFilterZ, SamplerState<11>);
  SetHandler(kD3DDevice_SetSamplerState_SeparateZFilterEnable, SamplerState<12>);
  SetHandler(kD3DDevice_SetSamplerState_MinMipLevel, SamplerState<13>);
  SetHandler(kD3DDevice_SetSamplerState_TrilinearThreshold, SamplerState<14>);
  SetHandler(kD3DDevice_SetSamplerState_AnisotropyBias, SamplerState<15>);
  SetHandler(kD3DDevice_SetSamplerState_HGradientExpBias, SamplerState<16>);
  SetHandler(kD3DDevice_SetSamplerState_VGradientExpBias, SamplerState<17>);
  SetHandler(kD3DDevice_SetSamplerState_WhiteBorderColorW, SamplerState<18>);
  SetHandler(kD3DDevice_SetSamplerState_PointBorderEnable, SamplerState<19>);

  SetHandler(kD3DDevice_SetClipPlane, ClipPlane);
  SetHandler(kD3DDevice_SetVertexShaderConstantB, ShaderConstants<0x2780, false>);
  SetHandler(kD3DDevice_SetPixelShaderConstantB, ShaderConstants<0x2790, false>);
  SetHandler(kD3DDevice_SetVertexShaderConstantI, ShaderConstants<0x27A0, true>);
  SetHandler(kD3DDevice_SetPixelShaderConstantI, ShaderConstants<0x27E0, true>);
}

void RegisterLayer1b() {
  SetHandler(kD3DDevice_SetTexture, Texture);
  SetHandler(kD3DDevice_SetTexture_Inline, Texture);
  SetHandler(kD3DDevice_SetStreamSource, StreamSource);
  SetHandler(kD3DDevice_SetIndices, Indices);
  SetHandler(kD3DDevice_SetVertexDeclaration, VertexDeclaration);
  SetHandler(kD3DDevice_SetViewport, Viewport);
  SetHandler(kD3D_SetViewport, ViewportFloats);
  SetHandler(kD3DDevice_SetScissorRect, ScissorRect);
  SetHandler(kD3DDevice_SetRenderTarget, RenderTarget);
  SetHandler(kD3DDevice_SetDepthStencilSurface, DepthStencilSurface);
  SetHandler(kD3DDevice_SetPredication, Predication);
  SetHandler(kD3DDevice_SetCommandBufferPredication, CommandBufferPredication);
  SetHandler(kD3DDevice_SetShaderGPRAllocation, ShaderGPRAllocation);
  SetHandler(kD3DDevice_SetScreenExtentQueryMode, ScreenExtentQueryMode);
  SetHandler(kD3DDevice_FlushHiZStencil, InvalidateConstantsCall);
  SetHandler(kD3DDevice_UnsetAll, UnsetAllCall);
}

}  // namespace

void RegisterStateHandlers() {
  SetSurfaceClipSink(RunSurfaceClip);
  RegisterLayer1a();
  RegisterLayer1b();
  RegisterLayer1c();
  RegisterResources();
  RegisterShaders();
  RegisterMisc();
  RegisterFrame();
  RegisterSwap();
  RegisterTiling();
  RegisterCommandBuffers();
  RegisterEdram();
  RegisterLeaves();
}

}  // namespace renut::d3d::port
