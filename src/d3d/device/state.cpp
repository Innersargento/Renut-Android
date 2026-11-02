// Layer 1a: the render and sampler state setters and getters, natively.
//
// Ported from reNut's d3d_native_state.cpp (masterspike52's reNut), which ported
// each function of N&B's state tables (101 render states, 20 sampler states) from
// the game's disassembly: same fields, same dirty bits, same order of writes. The
// register packets N&B's D3D would emit into the ring are left out, as in reNut;
// the native front end flushes from the shadow at draw time.
//
// Changed from the original: the guest-memory accessors (device.h), and the
// CDevice-private offsets, translated to this release's layout with Layout().
#include "d3d/device/internal.h"
#include "d3d/device/state.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace renut::d3d::port {

namespace {

constexpr float kInv255 = 0.003921568859368563f;  // flt_82000D08

// D3DMAXANISOTROPY to the fetch constant's aniso filter (dword_82003E38).
constexpr std::array<u32, 17> kAnisoFilter = {0, 0, 2, 2, 3, 3, 3, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5};

// ---------------------------------------------------------------------------
// Blend
// ---------------------------------------------------------------------------

// RB_BLENDCONTROL from the blend state: without separate alpha blending the
// alpha fields repeat the color ones.
u32 BlendControl(u32 blend, bool separate_alpha) {
  if (separate_alpha)
    return blend;
  u32 alpha = (blend & 0x1010) | ((blend & 0xFFFF) << 4);
  alpha = (alpha << 12) & 0xFFFF0000u;
  alpha &= ~0x00100000u;
  alpha &= ~0x10000000u;
  return alpha | (blend & 0xFFFF);
}

void StoreBlendControl(Device d, u32 value, bool separate_order) {
  // The two orders N&B's setters store in; the values are the same.
  if (separate_order) {
    d.SetW(off::BlendControl0, value);
    d.SetW(off::BlendControl1, value);
    d.SetW(off::BlendControl2, value);
    d.SetW(off::BlendControl3, value);
  } else {
    d.SetW(off::BlendControl2, value);
    d.SetW(off::BlendControl0, value);
    d.SetW(off::BlendControl1, value);
    d.SetW(off::BlendControl3, value);
  }
  d.Dirty(kPendingStates, 0x400 | 4 | 2 | 1);
}

// SrcBlend/DestBlend/BlendOp (color): applied while blending is enabled.
void SetBlendColorField(Device d, u32 value, u32 shift, u32 width) {
  d.SetW(kBlendState, Insert(d.W(kBlendState), value, shift, width));
  const u32 flags = d.W(kBlendFlags);
  if (!(flags & 0x80000000u))
    return;
  StoreBlendControl(d, BlendControl(d.W(kBlendState), (flags & 0x40000000u) != 0), true);
}

// The alpha fields: applied while blending with separate alpha.
void SetBlendAlphaField(Device d, u32 value, u32 shift, u32 width) {
  const u32 blend = Insert(d.W(kBlendState), value, shift, width);
  d.SetW(kBlendState, blend);
  const u32 flags = d.W(kBlendFlags);
  if (!(flags & 0x80000000u) || !(flags & 0x40000000u))
    return;
  StoreBlendControl(d, blend, true);
}

// ---------------------------------------------------------------------------
// Render states (off_82E506F0 order)
// ---------------------------------------------------------------------------

void RsNone(Device, u32) {}

void RsZEnable(Device d, u32 v) {  // 0x8264C140
  d.SetW(kZEnable, v);
  if (!d.W(off::DepthStencil))
    v = 0;
  d.SetW(off::DepthControl, Insert(d.W(off::DepthControl), v, 1, 1));
  d.Dirty(kPendingStates, 0x800 | 0x20000);
}
void RsZFunc(Device d, u32 v) {  // 0x821FB7B0
  d.SetW(off::DepthControl, Insert(d.W(off::DepthControl), v, 4, 3));
  d.Dirty(kPendingStates, 0x800 | 0x20000);
}
void RsZWriteEnable(Device d, u32 v) {  // 0x8264C180
  d.SetW(off::DepthControl, Insert(d.W(off::DepthControl), v, 2, 1));
  d.Dirty(kPendingStates, 0x800);
}
void RsFillMode(Device d, u32 v) {  // 0x8228F330
  d.SetW(off::ModeControl, Insert(d.W(off::ModeControl), v, 3, 8));
  d.Dirty(kPendingStates, 0x40);
}
void RsCullMode(Device d, u32 v) {  // 0x8228F350
  d.SetW(off::ModeControl, (d.W(off::ModeControl) & ~7u) | (v & 7));
  d.Dirty(kPendingStates, 0x40);
}
void RsAlphaBlendEnable(Device d, u32 v) {  // 0x8222A718
  const u32 flags = Insert(d.W(kBlendFlags), v, 31, 1);
  d.SetW(kBlendFlags, flags);
  u32 control = BlendControl(d.W(kBlendState), (flags & 0x40000000u) != 0);
  if (!v)
    control = 0x10001;
  StoreBlendControl(d, control, false);
}
void RsSeparateAlphaBlendEnable(Device d, u32 v) {  // 0x8222A690
  const u32 flags = Insert(d.W(kBlendFlags), v, 30, 1);
  d.SetW(kBlendFlags, flags);
  u32 control = BlendControl(d.W(kBlendState), v != 0);
  if (!(flags >> 31))
    control = 0x10001;
  StoreBlendControl(d, control, true);
}
void RsBlendFactor(Device d, u32 v) {  // 0x8226E8E0
  d.SetF(0x28E8, float(v & 0xFF) * kInv255);           // blue
  d.SetF(0x28E4, float((v >> 8) & 0xFF) * kInv255);    // green
  d.SetF(0x28E0, float((v >> 16) & 0xFF) * kInv255);   // red
  d.SetF(0x28EC, float(v >> 24) * kInv255);            // alpha
  d.Dirty(kPendingStates, u64(0xF) << 33);
}
void RsSrcBlend(Device d, u32 v) {  // 0x82230090
  SetBlendColorField(d, v, 0, 5);
}
void RsDestBlend(Device d, u32 v) {  // 0x822307B8
  SetBlendColorField(d, v, 8, 5);
}
void RsBlendOp(Device d, u32 v) {  // 0x82226380
  SetBlendColorField(d, v, 5, 3);
}
void RsSrcBlendAlpha(Device d, u32 v) {  // 0x822672B8
  SetBlendAlphaField(d, v, 16, 5);
}
void RsDestBlendAlpha(Device d, u32 v) {  // 0x822208E8
  SetBlendAlphaField(d, v, 24, 5);
}
void RsBlendOpAlpha(Device d, u32 v) {  // 0x8221F5F8
  SetBlendAlphaField(d, v, 21, 3);
}
void RsAlphaTestEnable(Device d, u32 v) {  // 0x8264BE98
  d.SetW(off::ColorControl, Insert(d.W(off::ColorControl), v, 3, 1));
  d.Dirty(kPendingStates, 0x200 | 0x40000);
}
void RsAlphaRef(Device d, u32 v) {  // 0x8264BF50
  d.SetF(0x2904, float(v) * kInv255);
  d.Dirty(kPendingStates, 0x8000000);
}
void RsAlphaFunc(Device d, u32 v) {  // 0x8264BFB8
  d.SetW(off::ColorControl, (d.W(off::ColorControl) & ~7u) | (v & 7));
  d.Dirty(kPendingStates, 0x200);
}
void RsStencilEnable(Device d, u32 v) {  // 0x8264C1C0
  d.SetW(kStencilEnable, v);
  if (!d.W(off::DepthStencil))
    v = 0;
  d.SetW(off::DepthControl, Insert(d.W(off::DepthControl), v, 0, 1));
  d.Dirty(kPendingStates, 0x800 | 0x20000);
}

template <u32 Shift, u64 Bits>
void RsDepthControlField(Device d, u32 v) {
  d.SetW(off::DepthControl, Insert(d.W(off::DepthControl), v, Shift, 3));
  d.Dirty(kPendingStates, Bits);
}

void RsTwoSidedStencilMode(Device d, u32 v) {  // 0x8264C200
  d.SetW(off::DepthControl, Insert(d.W(off::DepthControl), v, 7, 1));
  d.Dirty(kPendingStates, 0x800 | 0x20000);
}
void RsStencilRef(Device d, u32 v) {  // 0x8264C3D8
  d.SetB(0x2903, u8(v));
  d.Dirty(kPendingStates, 0x10000000);
}
void RsStencilMask(Device d, u32 v) {  // 0x8264C3F8
  d.SetB(0x2902, u8(v));
  d.Dirty(kPendingStates, 0x10000000);
}
void RsStencilWriteMask(Device d, u32 v) {  // 0x8264C418
  d.SetB(0x2901, u8(v));
  d.Dirty(kPendingStates, 0x10000000);
}
void RsCCWStencilRef(Device d, u32 v) {  // 0x8264C438
  d.SetB(0x28FF, u8(v));
  d.Dirty(kPendingStates, 0x20000000);
}
void RsCCWStencilMask(Device d, u32 v) {  // 0x8264C458
  d.SetB(0x28FE, u8(v));
  d.Dirty(kPendingStates, 0x20000000);
}
void RsCCWStencilWriteMask(Device d, u32 v) {  // 0x8264C478
  d.SetB(0x28FD, u8(v));
  d.Dirty(kPendingStates, 0x20000000);
}
void RsClipPlaneEnable(Device d, u32 v) {  // 0x8228F370
  d.SetW(off::ClipControl, (d.W(off::ClipControl) & ~0x3Fu) | v);
  d.SetW(0x28B4, v ? 0x1000u : 0u);
  d.Dirty(kPendingStates, 0x80 | Bit(44));
}
void RsPointSize(Device d, u32 v) {  // 0x8264C6C0
  d.SetW(kPointSize, v);
  const u16 size = u16(TruncateToI32(AsFloat(v) * 8.0f));
  d.SetH(0x2966, size);
  d.SetH(0x2964, size);
  d.Dirty(kPendingFetch, Bit(54));
}
void RsPointSizeMin(Device d, u32 v) {  // 0x8264C720
  d.SetW(kPointSizeMin, v);
  d.SetH(0x296A, u16(TruncateToI32(AsFloat(v) * 16.0f)));
  d.Dirty(kPendingFetch, Bit(53));
}
void RsPointSpriteEnable(Device d, u32 v) {  // 0x8264C6B0
  d.SetW(kPointSpriteEnable, v);
}
void RsPointSizeMax(Device d, u32 v) {  // 0x822687D0
  d.SetW(kPointSizeMax, v);
  d.SetH(0x2968, u16(TruncateToI32(AsFloat(v) * 16.0f)));
  d.Dirty(kPendingFetch, Bit(53));
}
void RsMultiSampleAntiAlias(Device d, u32 v) {  // 0x8264C598
  d.SetW(off::ModeControl, Insert(d.W(off::ModeControl), v, 15, 1));
  d.Dirty(kPendingStates, 0x40);
}
void RsMultiSampleMask(Device d, u32 v) {  // 0x8264C5C8
  d.SetW(0x2A00, v & 0xFFFF);
  d.Dirty(kPendingMisc, 0x80000);
}
void RsScissorTestEnable(Device d, u32 v) {  // 0x8264C4A8
  d.SetW(kScissorTestEnable, v);
  SetScissorRect(d, i32(d.W(off::ScissorRect)), i32(d.W(off::ScissorRect + 4)),
                 i32(d.W(off::ScissorRect + 8)), i32(d.W(off::ScissorRect + 12)));
}
void RsSlopeScaleDepthBias(Device d, u32 v) {  // 0x8264C4C0
  const float scale = AsFloat(v) * 16.0f;
  u32 mode = d.W(off::ModeControl);
  mode = Insert(mode, (scale != 0.0f || d.F(0x2A54) != 0.0f) ? 1 : 0, 11, 1);
  const bool back = scale != 0.0f || d.F(0x2A5C) != 0.0f;
  d.SetF(0x2A50, scale);
  mode = Insert(mode, back ? 1 : 0, 12, 1);
  d.SetF(0x2A58, scale);
  d.SetW(off::ModeControl, mode);
  d.Dirty(kPendingMisc, Bit(45) | Bit(43));
  d.Dirty(kPendingStates, 0x40);
}
void RsDepthBias(Device d, u32 v) {  // 0x821FB718
  const float bias = AsFloat(v);
  u32 mode = d.W(off::ModeControl);
  mode = Insert(mode, (d.F(0x2A50) != 0.0f || bias != 0.0f) ? 1 : 0, 11, 1);
  const bool back = d.F(0x2A58) != 0.0f || bias != 0.0f;
  d.SetF(0x2A54, bias);
  mode = Insert(mode, back ? 1 : 0, 12, 1);
  d.SetF(0x2A5C, bias);
  d.SetW(off::ModeControl, mode);
  d.Dirty(kPendingMisc, Bit(44) | Bit(42));
  d.Dirty(kPendingStates, 0x40);
}

template <u32 Target>
void RsColorWriteEnable(Device d, u32 v) {  // 0x821FB6E0, 0x8264C5F0, 0x8264C630, 0x8264C670
  d.SetW(kColorWriteEnable + Target * 4, v);
  if (!d.W(off::RenderTargets + Target * 4))
    v = 0;
  d.SetW(off::ColorMask, Insert(d.W(off::ColorMask), v, Target * 4, 4));
  d.Dirty(kPendingStates, Bit(37));
}

void RsTessellationMode(Device d, u32 v) {  // 0x8264CDC0
  d.SetW(0x2978, (d.W(0x2978) & ~3u) | (v & 3));
  d.Dirty(kPendingFetch, Bit(49));
}
void RsMinTessellationLevel(Device d, u32 v) {  // 0x8264CD50
  d.SetW(0x2980, v);
  d.Dirty(kPendingFetch, Bit(47));
}
void RsMaxTessellationLevel(Device d, u32 v) {  // 0x8264CD88
  d.SetW(0x297C, v);
  d.Dirty(kPendingFetch, Bit(48));
}

// Wrap0-7 in PA_SU_WRAPPING0 (0x292C), Wrap8-15 in WRAPPING1 (0x2930). The
// first of each word ORs the value in unmasked, as N&B's code does.
template <u32 Index>
void RsWrap(Device d, u32 v) {  // 0x8264C780 + ...
  constexpr u32 kWord = Index < 8 ? 0x292C : 0x2930;
  constexpr u32 kShift = (Index % 8) * 4;
  u32 word = d.W(kWord);
  if constexpr (kShift == 0)
    word = (word & ~0xFu) | v;
  else if constexpr (kShift == 28)
    word = Insert(word, v, 28, 4);
  else
    word = (word & ~(0xFu << kShift)) | (v << kShift);
  d.SetW(kWord, word);
  d.Dirty(kPendingStates, Index < 8 ? 0x2000 : 0x1000);
}

void RsViewportEnable(Device d, u32 v) {  // 0x82205A80
  d.SetW(0x294C, v ? 0x43Fu : 0x400u);
  d.SetW(off::ClipControl, Insert(d.W(off::ClipControl), v ? 0 : 1, 16, 1));
  d.Dirty(kPendingStates, 0x20 | 0x80);
}

template <u32 Target>
void RsHighPrecisionBlendEnable(Device d, u32 v) {  // 0x8264CAF0 + 0x98 * Target
  d.SetW(kHighPrecisionBlend + Target * 4, v);
  if (!d.W(off::RenderTargets + Target * 4))
    return;
  ApplyHighPrecisionBlend(d, Target ? Target + 1 : 0, v);
}

void RsHalfPixelOffset(Device d, u32 v) {  // 0x8264CDF8
  d.SetW(0x29C0, (d.W(0x29C0) & ~1u) | (v & 1));
  d.Dirty(kPendingMisc, Bit(35));
}
void RsPrimitiveResetEnable(Device d, u32 v) {  // 0x8264CE30
  d.SetW(off::ModeControl, Insert(d.W(off::ModeControl), v, 21, 1));
  d.Dirty(kPendingStates, 0x40);
}
void RsPrimitiveResetIndex(Device d, u32 v) {  // 0x8264CE60
  d.SetW(0x28D8, v);
  d.Dirty(kPendingStates, Bit(38));
}
void RsAlphaToMaskEnable(Device d, u32 v) {  // 0x8264CE88
  d.SetW(off::ColorControl, Insert(d.W(off::ColorControl), v, 4, 1));
  d.Dirty(kPendingStates, 0x200);
}
void RsAlphaToMaskOffsets(Device d, u32 v) {  // 0x8264CEB8
  d.SetW(off::ColorControl, Insert(d.W(off::ColorControl), v, 24, 8));
  d.Dirty(kPendingStates, 0x200);
}
void RsGuardBandX(Device d, u32 v) {  // 0x8264CEF0
  d.SetW(0x29CC, v);
  d.Dirty(kPendingMisc, Bit(32));
}
void RsGuardBandY(Device d, u32 v) {  // 0x8264CF20
  d.SetW(0x29C4, v);
  d.Dirty(kPendingMisc, Bit(34));
}
void RsDiscardBandX(Device d, u32 v) {  // 0x8264CF50
  d.SetW(0x29D0, v);
  d.Dirty(kPendingMisc, 0x80000000u);
}
void RsDiscardBandY(Device d, u32 v) {  // 0x8264CF78
  d.SetW(0x29C8, v);
  d.Dirty(kPendingMisc, Bit(33));
}
void RsHiStencilEnable(Device d, u32 v) {  // 0x8264CFE8
  d.SetW(off::HiControl, Insert(d.W(off::HiControl), v, 3, 1));
  d.Dirty(kPendingStates, 0x100);
}
void RsHiStencilWriteEnable(Device d, u32 v) {  // 0x8264D018
  d.SetW(off::HiControl, Insert(d.W(off::HiControl), v, 2, 1));
  d.Dirty(kPendingStates, 0x100);
}
void RsHiStencilFunc(Device d, u32 v) {  // 0x8264D048
  d.SetW(off::HiControl, Insert(d.W(off::HiControl), v, 5, 1));
  d.Dirty(kPendingStates, 0x100);
}
void RsHiStencilRef(Device d, u32 v) {  // 0x8264D078
  d.SetB(0x2942, u8(v));
  d.Dirty(kPendingStates, 0x100);
}
void RsPresentInterval(Device d, u32 v) {  // 0x8264D098
  d.SetW(kPresentInterval, v);
}
void RsPresentImmediateThreshold(Device d, u32 v) {  // 0x8264D0A8
  d.SetW(kBlendFlags, Insert(d.W(kBlendFlags), v, 23, 7));
}
void RsHiZEnable(Device d, u32 v) {  // 0x8264CFA8
  d.SetW(kBlendFlags, Insert(d.W(kBlendFlags), v, 20, 3));
  d.Dirty(kPendingStates, 0x20000);
}
void RsHiZWriteEnable(Device d, u32 v) {  // 0x822899D0
  const u32 flags = Insert(d.W(kBlendFlags), v, 17, 3);
  d.SetW(kBlendFlags, flags);
  u64 predicated = d.Q(off::PredicatedPending2);
  if (!(d.B(off::Flags3) & 0x20) && (flags & 0xE0000) == 0x40000)
    predicated &= ~u64(0x100);
  else
    predicated |= 0x100;
  d.SetQ(off::PredicatedPending2, predicated);
  d.Dirty(kPendingStates, 0x100);
}
void RsLastPixel(Device d, u32 v) {  // 0x8264C060
  d.SetW(0x29B8, Insert(d.W(0x29B8), v, 10, 1));
  d.Dirty(kPendingMisc, Bit(37));
}
void RsLineWidth(Device d, u32 v) {  // 0x8264C098
  d.SetH(0x296E, u16(TruncateToI64(AsFloat(v) * 8.0f)));
  d.Dirty(kPendingFetch, Bit(52));
}
void RsBuffer2Frames(Device d, u32 v) {  // 0x8264C110
  const u32 flags = d.W(kBehaviorFlags2);
  d.SetW(kBehaviorFlags2, v ? (flags | 1) : (flags & ~1u));
}

using RenderStateFn = void (*)(Device, u32);
constexpr u32 kRenderStates = 101;
constexpr RenderStateFn kRenderStateFns[kRenderStates] = {
    RsNone, RsNone, RsNone, RsNone, RsNone, RsNone, RsNone, RsNone, RsNone, RsNone,  // 0-9
    RsZEnable,                                   // 10
    RsZFunc,                                     // 11
    RsZWriteEnable,                              // 12
    RsFillMode,                                  // 13
    RsCullMode,                                  // 14
    RsAlphaBlendEnable,                          // 15
    RsSeparateAlphaBlendEnable,                  // 16
    RsBlendFactor,                               // 17
    RsSrcBlend,                                  // 18
    RsDestBlend,                                 // 19
    RsBlendOp,                                   // 20
    RsSrcBlendAlpha,                             // 21
    RsDestBlendAlpha,                            // 22
    RsBlendOpAlpha,                              // 23
    RsAlphaTestEnable,                           // 24
    RsAlphaRef,                                  // 25
    RsAlphaFunc,                                 // 26
    RsStencilEnable,                             // 27
    RsTwoSidedStencilMode,                       // 28
    RsDepthControlField<11, 0x800 | 0x20000>,    // 29 StencilFail
    RsDepthControlField<17, 0x800 | 0x20000>,    // 30 StencilZFail
    RsDepthControlField<14, 0x800>,              // 31 StencilPass
    RsDepthControlField<8, 0x800>,               // 32 StencilFunc
    RsStencilRef,                                // 33
    RsStencilMask,                               // 34
    RsStencilWriteMask,                          // 35
    RsDepthControlField<23, 0x800 | 0x20000>,    // 36 CCWStencilFail
    RsDepthControlField<29, 0x800 | 0x20000>,    // 37 CCWStencilZFail
    RsDepthControlField<26, 0x800>,              // 38 CCWStencilPass
    RsDepthControlField<20, 0x800>,              // 39 CCWStencilFunc
    RsCCWStencilRef,                             // 40
    RsCCWStencilMask,                            // 41
    RsCCWStencilWriteMask,                       // 42
    RsClipPlaneEnable,                           // 43
    RsPointSize,                                 // 44
    RsPointSizeMin,                              // 45
    RsPointSpriteEnable,                         // 46
    RsPointSizeMax,                              // 47
    RsMultiSampleAntiAlias,                      // 48
    RsMultiSampleMask,                           // 49
    RsScissorTestEnable,                         // 50
    RsSlopeScaleDepthBias,                       // 51
    RsDepthBias,                                 // 52
    RsColorWriteEnable<0>,                       // 53
    RsColorWriteEnable<1>,                       // 54
    RsColorWriteEnable<2>,                       // 55
    RsColorWriteEnable<3>,                       // 56
    RsTessellationMode,                          // 57
    RsMinTessellationLevel,                      // 58
    RsMaxTessellationLevel,                      // 59
    RsWrap<0>, RsWrap<1>, RsWrap<2>, RsWrap<3>, RsWrap<4>, RsWrap<5>, RsWrap<6>, RsWrap<7>,
    RsWrap<8>, RsWrap<9>, RsWrap<10>, RsWrap<11>, RsWrap<12>, RsWrap<13>, RsWrap<14>,
    RsWrap<15>,                                  // 60-75
    RsViewportEnable,                            // 76
    RsHighPrecisionBlendEnable<0>,               // 77
    RsHighPrecisionBlendEnable<1>,               // 78
    RsHighPrecisionBlendEnable<2>,               // 79
    RsHighPrecisionBlendEnable<3>,               // 80
    RsHalfPixelOffset,                           // 81
    RsPrimitiveResetEnable,                      // 82
    RsPrimitiveResetIndex,                       // 83
    RsAlphaToMaskEnable,                         // 84
    RsAlphaToMaskOffsets,                        // 85
    RsGuardBandX,                                // 86
    RsGuardBandY,                                // 87
    RsDiscardBandX,                              // 88
    RsDiscardBandY,                              // 89
    RsHiStencilEnable,                           // 90
    RsHiStencilWriteEnable,                      // 91
    RsHiStencilFunc,                             // 92
    RsHiStencilRef,                              // 93
    RsPresentInterval,                           // 94
    RsPresentImmediateThreshold,                 // 95
    RsHiZEnable,                                 // 96
    RsHiZWriteEnable,                            // 97
    RsLastPixel,                                 // 98
    RsLineWidth,                                 // 99
    RsBuffer2Frames,                             // 100
};

// ---------------------------------------------------------------------------
// Sampler states (off_82E50BD0 order)
// ---------------------------------------------------------------------------

// The texture's Z filter (fetch dword 4, bits 0-1) from the min/mag filters
// in dword 3, or from the separate Z filter state when that is enabled.
u32 ZFilterBits(u32 dword3, u8 z_state) {
  const u32 folded = (dword3 & 0x80080000u) | ((dword3 >> 1) & 0x7FF7FFFFu);
  const u32 separate_mask = (u32(z_state) >> 2) - 1;  // 0 when separate, ~0 when not
  return ((Extract(folded, 19, 12) & separate_mask) + (u32(z_state) & ~separate_mask)) & 3;
}

void SsAddress(Device d, u32 s, u32 v, u32 shift) {
  d.SetW(Fetch(s, 0), Insert(d.W(Fetch(s, 0)), v, shift, 3));
  d.Dirty(kPendingFetch, FetchBit(s));
}
void SsAddressU(Device d, u32 s, u32 v) {  // 0x8227D2D0
  SsAddress(d, s, v, 10);
}
void SsAddressV(Device d, u32 s, u32 v) {  // 0x8220C4E0
  SsAddress(d, s, v, 13);
}
void SsAddressW(Device d, u32 s, u32 v) {  // 0x8264D630
  SsAddress(d, s, v, 16);
}
void SsBorderColor(Device d, u32 s, u32 v) {  // 0x8264D590
  d.SetW(Fetch(s, 5), (d.W(Fetch(s, 5)) & ~3u) | (v ? 1u : 0u));
  d.Dirty(kPendingFetch, FetchBit(s));
}
// MagFilter (filter at dword 3 bits 19-20, aniso flag at dword 4 bit 10) and
// MinFilter (21-22, flag at 11); the aniso filter (dword 3 bits 25-27) is set
// while either is anisotropic.
void SsFilter(Device d, u32 s, u32 v, bool mag) {  // 0x8222F1B8 / 0x8222F280
  const u32 aniso = kAnisoFilter[d.B(kMaxAnisotropy + s) % kAnisoFilter.size()];
  const u32 anisotropic = v >> 2;
  const u32 dword4 = d.W(Fetch(s, 4));
  const u32 other = Extract(dword4, mag ? 11 : 10, 1);
  const u32 new4 = Insert(dword4, anisotropic, mag ? 10 : 11, 1);
  d.SetW(Fetch(s, 4), new4);
  const u32 aniso_field = aniso & ~((other | anisotropic) - 1);
  u32 dword3 = d.W(Fetch(s, 3));
  if (mag) {
    const u32 bits = (aniso_field << 6) | anisotropic | v;
    dword3 = Insert(dword3, bits, 19, 2);
    dword3 = (dword3 & ~0x0E000000u) | ((bits << 19) & 0x0E000000u);
  } else {
    const u32 bits = (aniso_field << 4) | anisotropic | v;
    dword3 = Insert(dword3, bits, 21, 2);
    dword3 = (dword3 & ~0x0E000000u) | ((bits << 21) & 0x0E000000u);
  }
  d.SetW(Fetch(s, 3), dword3);
  d.SetW(Fetch(s, 4), (new4 & ~3u) | ZFilterBits(dword3, d.B(kZFilter + s)));
  d.Dirty(kPendingFetch, FetchBit(s));
}
void SsMagFilter(Device d, u32 s, u32 v) {
  SsFilter(d, s, v, true);
}
void SsMinFilter(Device d, u32 s, u32 v) {
  SsFilter(d, s, v, false);
}
void SsMipFilter(Device d, u32 s, u32 v) {  // 0x8228F2F0
  d.SetW(Fetch(s, 3), Insert(d.W(Fetch(s, 3)), v, 23, 2));
  d.Dirty(kPendingFetch, FetchBit(s));
}
void SsMipMapLodBias(Device d, u32 s, u32 v) {  // 0x8264D460
  const u32 bias = u32(TruncateToI64(AsFloat(v) * 32.0f));
  d.SetW(Fetch(s, 4), Insert(d.W(Fetch(s, 4)), bias, 12, 10));
  d.Dirty(kPendingFetch, FetchBit(s));
}
// MaxMipLevel / MinMipLevel, limited to the bound texture's own mip range.
void SsMaxMipLevel(Device d, u32 s, u32 v) {  // 0x8222F590
  if (const u32 texture = d.W(off::Textures + s * 4)) {
    u32 level = Extract(d.GuestW(texture + 0x2C), 2, 4);
    if (level <= v)
      level = v;
    d.SetW(Fetch(s, 4), Insert(d.W(Fetch(s, 4)), level, 2, 4));
    d.Dirty(kPendingFetch, FetchBit(s));
  }
  d.SetB(kMaxMipLevel + s, u8(v));
}
void SsMinMipLevel(Device d, u32 s, u32 v) {  // 0x8264D510
  if (const u32 texture = d.W(off::Textures + s * 4)) {
    u32 level = Extract(d.GuestW(texture + 0x2C), 6, 4);
    if (level >= v)
      level = v;
    d.SetW(Fetch(s, 4), Insert(d.W(Fetch(s, 4)), level, 6, 4));
    d.Dirty(kPendingFetch, FetchBit(s));
  }
  d.SetB(kMinMipLevel + s, u8(v));
}
void SsMaxAnisotropy(Device d, u32 s, u32 v) {  // 0x8264D340
  const u32 dword4 = d.W(Fetch(s, 4));
  if ((dword4 & 0x800) || (dword4 & 0x400)) {
    d.SetW(Fetch(s, 3), Insert(d.W(Fetch(s, 3)), kAnisoFilter[v % kAnisoFilter.size()], 25, 3));
    d.Dirty(kPendingFetch, FetchBit(s));
  }
  d.SetB(kMaxAnisotropy + s, u8(v));
}
// MagFilterZ / MinFilterZ / SeparateZFilterEnable: bits 0 / 1 / 2 of the
// sampler's Z filter state, then the fetch constant's Z filter.
void SsZFilterState(Device d, u32 s, u32 v, u32 bit) {  // 0x8264D1E0 / 0x8264D100 / 0x8264D298
  const u8 z_state = u8((d.B(kZFilter + s) & ~(1u << bit)) | (v << bit));
  const u32 dword3 = d.W(Fetch(s, 3));
  d.SetW(Fetch(s, 4), (d.W(Fetch(s, 4)) & ~3u) | ZFilterBits(dword3, z_state));
  d.SetB(kZFilter + s, z_state);
  d.Dirty(kPendingFetch, FetchBit(s));
}
void SsMagFilterZ(Device d, u32 s, u32 v) {
  SsZFilterState(d, s, v, 0);
}
void SsMinFilterZ(Device d, u32 s, u32 v) {
  SsZFilterState(d, s, v, 1);
}
void SsSeparateZFilterEnable(Device d, u32 s, u32 v) {
  SsZFilterState(d, s, v, 2);
}
void SsTrilinearThreshold(Device d, u32 s, u32 v) {  // 0x8264D680
  d.SetW(Fetch(s, 5), Insert(d.W(Fetch(s, 5)), v, 3, 2));
  d.Dirty(kPendingFetch, FetchBit(s));
}
void SsAnisotropyBias(Device d, u32 s, u32 v) {  // 0x8264D3B8
  const u32 bias = u32(TruncateToI64(AsFloat(v) * -8.0f));
  d.SetW(Fetch(s, 5), Insert(d.W(Fetch(s, 5)), bias, 5, 4));
  d.Dirty(kPendingFetch, FetchBit(s));
}
void SsHGradientExpBias(Device d, u32 s, u32 v) {  // 0x8264D6D8
  d.SetW(Fetch(s, 4), Insert(d.W(Fetch(s, 4)), v, 22, 5));
  d.Dirty(kPendingFetch, FetchBit(s));
}
void SsVGradientExpBias(Device d, u32 s, u32 v) {  // 0x8264D730
  d.SetW(Fetch(s, 4), Insert(d.W(Fetch(s, 4)), v, 27, 5));
  d.Dirty(kPendingFetch, FetchBit(s));
}
void SsWhiteBorderColorW(Device d, u32 s, u32 v) {  // 0x8264D788
  d.SetW(Fetch(s, 5), Insert(d.W(Fetch(s, 5)), v, 2, 1));
  d.Dirty(kPendingFetch, FetchBit(s));
}
void SsPointBorderEnable(Device d, u32 s, u32 v) {  // 0x8264D7E0
  d.SetW(Fetch(s, 1), Insert(d.W(Fetch(s, 1)), v ? 0 : 1, 11, 1));
  d.Dirty(kPendingFetch, FetchBit(s));
}

using SamplerStateFn = void (*)(Device, u32, u32);
constexpr u32 kSamplerStates = 20;
constexpr SamplerStateFn kSamplerStateFns[kSamplerStates] = {
    SsAddressU,             // 0
    SsAddressV,             // 1
    SsAddressW,             // 2
    SsBorderColor,          // 3
    SsMagFilter,            // 4
    SsMinFilter,            // 5
    SsMipFilter,            // 6
    SsMipMapLodBias,        // 7
    SsMaxMipLevel,          // 8
    SsMaxAnisotropy,        // 9
    SsMagFilterZ,           // 10
    SsMinFilterZ,           // 11
    SsSeparateZFilterEnable,  // 12
    SsMinMipLevel,          // 13
    SsTrilinearThreshold,   // 14
    SsAnisotropyBias,       // 15
    SsHGradientExpBias,     // 16
    SsVGradientExpBias,     // 17
    SsWhiteBorderColorW,    // 18
    SsPointBorderEnable,    // 19
};

}  // namespace

// ---------------------------------------------------------------------------

void SetRenderState(Device device, u32 state, u32 value) {
  if (device && state < kRenderStates)
    kRenderStateFns[state](device, value);
}

void SetSamplerState(Device device, u32 sampler, u32 state, u32 value) {
  if (device && sampler < kTextureSamplers && state < kSamplerStates)
    kSamplerStateFns[state](device, sampler, value);
}

// The getters (0x8264BE78-0x8264D840), each a read of the state's storage.
// Entries 0-9 of the table share a getter returning 0.
u32 GetRenderState(Device d, u32 state) {
  if (!d)
    return 0;
  auto bits = [&d](u32 offset, u32 shift, u32 width) { return Extract(d.W(offset), shift, width); };
  // A [0, 1] float as a byte: fmadds by 255 and 0.5 (flt_821258F4, flt_82000C74), fctidz.
  auto byte = [](float v) { return u32(TruncateToI64(std::fma(v, 255.0f, 0.5f))); };
  switch (state) {
    case 10: return d.W(kZEnable);
    case 11: return bits(off::DepthControl, 4, 3);   // ZFunc
    case 12: return bits(off::DepthControl, 2, 1);   // ZWriteEnable
    case 13: return bits(off::ModeControl, 3, 8);    // FillMode
    case 14: return bits(off::ModeControl, 0, 3);    // CullMode
    case 15: return d.W(kBlendFlags) >> 31;          // AlphaBlendEnable
    case 16: return bits(kBlendFlags, 30, 1);        // SeparateAlphaBlendEnable
    case 17: {  // BlendFactor (sub_8264BFE8): ARGB from the RGBA floats
      const u32 r = byte(d.F(0x28E0)), g = byte(d.F(0x28E4)), b = byte(d.F(0x28E8));
      const u32 a = byte(d.F(0x28EC));
      return (a << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
    }
    case 18: return bits(kBlendState, 0, 5);         // SrcBlend
    case 19: return bits(kBlendState, 8, 5);         // DestBlend
    case 20: return bits(kBlendState, 5, 3);         // BlendOp
    case 21: return bits(kBlendState, 16, 5);        // SrcBlendAlpha
    case 22: return bits(kBlendState, 24, 5);        // DestBlendAlpha
    case 23: return bits(kBlendState, 21, 3);        // BlendOpAlpha
    case 24: return bits(off::ColorControl, 3, 1);   // AlphaTestEnable
    case 25: return byte(d.F(0x2904));               // AlphaRef (sub_8264BF88)
    case 26: return bits(off::ColorControl, 0, 3);   // AlphaFunc
    case 27: return d.W(kStencilEnable);
    case 28: return bits(off::DepthControl, 7, 1);   // TwoSidedStencilMode
    case 29: return bits(off::DepthControl, 11, 3);  // StencilFail
    case 30: return bits(off::DepthControl, 17, 3);  // StencilZFail
    case 31: return bits(off::DepthControl, 14, 3);  // StencilPass
    case 32: return bits(off::DepthControl, 8, 3);   // StencilFunc
    case 33: return d.B(0x2903);                     // StencilRef
    case 34: return d.B(0x2902);                     // StencilMask
    case 35: return d.B(0x2901);                     // StencilWriteMask
    case 36: return bits(off::DepthControl, 23, 3);  // CCWStencilFail
    case 37: return bits(off::DepthControl, 29, 3);  // CCWStencilZFail
    case 38: return bits(off::DepthControl, 26, 3);  // CCWStencilPass
    case 39: return bits(off::DepthControl, 20, 3);  // CCWStencilFunc
    case 40: return d.B(0x28FF);                     // CCWStencilRef
    case 41: return d.B(0x28FE);                     // CCWStencilMask
    case 42: return d.B(0x28FD);                     // CCWStencilWriteMask
    case 43: return bits(off::ClipControl, 0, 6);    // ClipPlaneEnable
    case 44: return d.W(kPointSize);
    case 45: return d.W(kPointSizeMin);
    case 46: return d.W(kPointSpriteEnable);
    case 47: return d.W(kPointSizeMax);
    case 48: return bits(off::ModeControl, 15, 1);   // MultiSampleAntiAlias
    case 49: return d.W(0x2A00);                     // MultiSampleMask
    case 50: return d.W(kScissorTestEnable);
    case 51: return std::bit_cast<u32>(d.F(0x2A50) * 0.0625f);  // SlopeScaleDepthBias (flt_82000B58)
    case 52: return d.W(0x2A54);                     // DepthBias
    case 53: case 54: case 55: case 56:              // ColorWriteEnable0-3
      return d.W(kColorWriteEnable + (state - 53) * 4);
    case 57: return bits(0x2978, 0, 2);              // TessellationMode
    case 58: return d.W(0x2980);                     // MinTessellationLevel
    case 59: return d.W(0x297C);                     // MaxTessellationLevel
    case 76: return bits(0x294C, 0, 1);              // ViewportEnable
    case 77: case 78: case 79: case 80:              // HighPrecisionBlendEnable0-3
      return d.W(kHighPrecisionBlend + (state - 77) * 4);
    case 81: return bits(0x29C0, 0, 1);              // HalfPixelOffset
    case 82: return bits(off::ModeControl, 21, 1);   // PrimitiveResetEnable
    case 83: return d.W(0x28D8);                     // PrimitiveResetIndex
    case 84: return bits(off::ColorControl, 4, 1);   // AlphaToMaskEnable
    case 85: return d.B(off::ColorControl);          // AlphaToMaskOffsets
    case 86: return d.W(0x29CC);                     // GuardBand_X
    case 87: return d.W(0x29C4);                     // GuardBand_Y
    case 88: return d.W(0x29D0);                     // DiscardBand_X
    case 89: return d.W(0x29C8);                     // DiscardBand_Y
    case 90: return bits(off::HiControl, 3, 1);      // HiStencilEnable
    case 91: return bits(off::HiControl, 2, 1);      // HiStencilWriteEnable
    case 92: return bits(off::HiControl, 5, 1);      // HiStencilFunc
    case 93: return d.B(off::HiControl + 2);         // HiStencilRef
    case 94: return d.W(kPresentInterval);
    case 95: return bits(kBlendFlags, 23, 7);        // PresentImmediateThreshold
    case 96: return bits(kBlendFlags, 20, 3);        // HiZEnable
    case 97: return bits(kBlendFlags, 17, 3);        // HiZWriteEnable
    case 98: return bits(0x29B8, 10, 1);             // LastPixel
    case 99: return std::bit_cast<u32>(float(d.H(0x296E)) * 0.125f);  // LineWidth (sub_8264C0D8)
    case 100: return bits(kBehaviorFlags2, 0, 1);    // Buffer2Frames
    default:
      if (state >= 60 && state <= 75) {  // Wrap0-15, a nibble each
        const u32 wrap = state - 60;
        return bits(0x292C + (wrap / 8) * 4, (wrap % 8) * 4, 4);
      }
      return 0;
  }
}

u32 GetSamplerState(Device d, u32 s, u32 state) {
  if (!d || s >= kTextureSamplers)
    return 0;
  auto fetch = [&](u32 dword) { return d.W(Fetch(s, dword)); };
  switch (state) {
    case 0: return Extract(fetch(0), 10, 3);         // AddressU
    case 1: return Extract(fetch(0), 13, 3);         // AddressV
    case 2: return Extract(fetch(0), 16, 3);         // AddressW
    case 3: return (fetch(5) & 3) ? ~0u : 0;         // BorderColor
    case 4:                                          // MagFilter: anisotropic, or the mag filter
      return Extract(fetch(4), 10, 1) ? 4 : Extract(fetch(3), 19, 2);
    case 5:                                          // MinFilter
      return Extract(fetch(4), 11, 1) ? 4 : Extract(fetch(3), 21, 2);
    case 6: return Extract(fetch(3), 23, 2);         // MipFilter
    case 7:                                          // MipMapLodBias: signed 10 bits, / 32
      return std::bit_cast<u32>(float(i32(fetch(4) << 10) >> 22) * 0.03125f);
    case 8: return d.B(kMaxMipLevel + s);
    case 9: return d.B(kMaxAnisotropy + s);
    case 10: return d.B(kZFilter + s) & 1;           // MagFilterZ
    case 11: return (d.B(kZFilter + s) >> 1) & 1;    // MinFilterZ
    case 12: return (d.B(kZFilter + s) >> 2) & 1;    // SeparateZFilterEnable
    case 13: return d.B(kMinMipLevel + s);
    case 14: return Extract(fetch(5), 3, 2);         // TrilinearThreshold
    case 15:                                         // AnisotropyBias (sub_8264D418): -signed 4 bits / 8
      return std::bit_cast<u32>(-(float(i32(fetch(5) << 23) >> 28) * 0.125f));
    case 16: return u32(i32(fetch(4) << 5) >> 27);   // HGradientExpBias
    case 17: return u32(i32(fetch(4)) >> 27);        // VGradientExpBias
    case 18: return Extract(fetch(5), 2, 1);         // WhiteBorderColorW
    case 19: return Extract(~fetch(1), 11, 1);       // PointBorderEnable
    default: return 0;
  }
}

// void D3DDevice_SetClipPlane(D3DDevice*, DWORD Index, const float *pPlane) [0x8264DAE8]
void SetClipPlane(Device d, u32 index, const u8* plane) {
  if (!d || !plane || index >= 6)
    return;
  for (u32 i = 0; i < 4; ++i)
    d.SetW(off::ClipPlanes + index * 16 + i * 4, LoadBE32(plane + i * 4));
  d.Dirty(4, u64(1) << (54 - index));
}

// The boolean and integer shader constants (D3DDevice_Set{Vertex,Pixel}
// ShaderConstant{B,I}): bits from 0x2780 (vertex) / 0x2790 (pixel), a word
// of x, y, z bytes each from 0x27A0 / 0x27E0. All below 0x2AC0, so the same on
// every release.
void SetShaderConstantsB(Device d, u32 first_word, u32 start, const u8* data, u32 count) {
  // 128 bits a stage: more would run into the next stage's words and the integers.
  if (!d || !data || start > 128 || count > 128 - start)
    return;
  for (u32 i = 0; i < count; ++i, ++start) {
    const u32 word = first_word + (start >> 5) * 4;
    const u32 bit = 1u << (start & 31);
    d.SetW(word, (d.W(word) & ~bit) | ((LoadBE32(data + i * 4) & 1) << (start & 31)));
  }
  d.Dirty(4, u64(1) << 56);
}

void SetShaderConstantsI(Device d, u32 first_word, u32 start, const u8* data, u32 count) {
  // 16 words a stage; the pixel ones end where the clip planes begin.
  if (!d || !data || start > 16 || count > 16 - start)
    return;
  for (u32 i = 0; i < count; ++i) {
    const u32 x = LoadBE32(data + i * 16) & 0xFF, y = LoadBE32(data + i * 16 + 4) & 0xFF;
    const u32 z = LoadBE32(data + i * 16 + 8) & 0xFF;
    d.SetW(first_word + (start + i) * 4, (z << 16) | (y << 8) | x);
  }
  d.Dirty(4, u64(1) << 56);
}

// RB_COLOR*_INFO's format for high precision blending (2, 3, 10 and 12 have
// an alternative), as HighPrecisionBlendEnable and SetRenderTarget apply it.
// 'slot' is the target's register (0, 2, 3, 4: DepthInfo sits at 1).
void ApplyHighPrecisionBlend(Device d, u32 slot, u32 enable) {
  const u32 offset = off::Color0Info + slot * 4;
  const u32 info = d.W(offset);
  const u32 format = Extract(info, 16, 4);
  if (format != 2 && format != 3 && format != 10 && format != 12)
    return;
  if ((Extract(info, 19, 1) ^ enable) == 0)
    return;
  const u32 enabled = ~(enable - 1);
  const u32 low = ((format >> 1) - 3) & (enable - 1);
  u32 value = ((enabled << 16) & ((format + 3) << 17)) | (low << 16);
  value = (info & ~0xF0000u) | (value & 0xF0000u);
  d.SetW(offset, value);
  d.Dirty(kPendingStates, Bit(56 - slot));
}

// void D3DDevice_SetScissorRect(D3DDevice*, const RECT*) [0x82223178]: the
// window scissor is the rect (while ScissorTestEnable) within the viewport.
// SetSurfaceClip (0x82223270) then writes the clip into the ring only (the native
// front end records it from its own hook).
void SetScissorRect(Device device, i32 left, i32 top, i32 right, i32 bottom) {
  i32 x1 = TruncateToI32(device.F(kViewport));
  i32 y1 = TruncateToI32(device.F(kViewport + 4));
  const i32 width = TruncateToI32(device.F(kViewport + 8));
  const i32 height = TruncateToI32(device.F(kViewport + 12));
  device.SetW(off::ScissorRect, u32(left));
  device.SetW(off::ScissorRect + 4, u32(top));
  device.SetW(off::ScissorRect + 8, u32(right));
  device.SetW(off::ScissorRect + 12, u32(bottom));
  i32 x2 = width + x1;
  i32 y2 = height + y1;
  if (device.W(kScissorTestEnable)) {
    if (x1 <= left)
      x1 = left;
    if (y1 <= top)
      y1 = top;
    if (x2 >= right)
      x2 = right;
    if (y2 >= bottom)
      y2 = bottom;
  }
  u32 tl = device.W(off::WindowScissorTL);
  u32 br = device.W(off::WindowScissorBR);
  tl = Insert(tl, u32(y1), 16, 15);
  br = Insert(br, u32(y2), 16, 15);
  tl = Insert(tl, u32(x1), 0, 15);
  br = Insert(br, u32(x2), 0, 15);
  device.SetW(off::WindowScissorTL, tl);
  device.SetW(off::WindowScissorBR, br);
  NotifySurfaceClip(device, x1, y1, x2, y2);
}

namespace {
SurfaceClipSink surface_clip_sink = nullptr;
}  // namespace

void SetSurfaceClipSink(SurfaceClipSink sink) { surface_clip_sink = sink; }

void NotifySurfaceClip(Device device, i32 x1, i32 y1, i32 x2, i32 y2) {
  if (surface_clip_sink)
    surface_clip_sink(device, x1, y1, x2, y2);
}

}  // namespace renut::d3d::port
