// Every Direct3D entry point of the game that reNut intercepts, written out.
//
// Included at the end of d3d_layer.cpp, which defines Taken/Observed/Forwarded.
// Each hook is a REX_HOOK_RAW over the recompiled function's own symbol; the
// generated body stays reachable as __imp__<symbol> and runs unless the front
// end took the call. The three names say what reNut does with the function:
//
//   Taken      the native front end replaces the call (it may still decline a
//              particular call, and then the original runs).
//   Observed   the original always runs; reNut reads or adjusts around it.
//   Forwarded  the original runs untouched. These are counted and probed for the
//              reports, and are mostly the state setters that fill the device's
//              shadow registers, which the native front end reads at draw time.
//
// All three share one body on purpose: the dispositions are documentation of the
// code in d3d/native/ (OnEnter, SingleTile) and d3d_layer.cpp (Leave), not a
// second switch. When one of those changes, move the function here. The list of
// names (enum Function) is d3d_functions.inc; a function must be in both.

//=============================================================================
// Taken: the native front end replaces these
//=============================================================================

REX_EXTERN(__imp__sub_826567C8);
REX_HOOK_RAW(sub_826567C8) {  // D3DDevice_DrawIndexedVertices
  // the draw becomes a draw record of the host list (d3d/native/)
  Taken(kD3DDevice_DrawIndexedVertices, __imp__sub_826567C8, ctx, base);
}

REX_EXTERN(__imp__sub_8222C7A0);
REX_HOOK_RAW(sub_8222C7A0) {  // D3DDevice_DrawVertices
  // the draw becomes a draw record of the host list
  Taken(kD3DDevice_DrawVertices, __imp__sub_8222C7A0, ctx, base);
}

REX_EXTERN(__imp__sub_8222CC50);
REX_HOOK_RAW(sub_8222CC50) {  // D3DDevice_BeginVertices
  // immediate-mode draw: the vertices go to fetch constant 95 from a scratch buffer, the draw is recorded
  Taken(kD3DDevice_BeginVertices, __imp__sub_8222CC50, ctx, base);
}

REX_EXTERN(__imp__sub_82656208);
REX_HOOK_RAW(sub_82656208) {  // D3DDevice_BeginIndexedVertices
  // immediate-mode indexed draw: scratch vertex and index buffers, the draw is recorded
  Taken(kD3DDevice_BeginIndexedVertices, __imp__sub_82656208, ctx, base);
}

REX_EXTERN(__imp__sub_82230838);
REX_HOOK_RAW(sub_82230838) {  // D3DDevice_DrawIndexedTessellatedVertices
  // the water: tessellation registers set in the shadow, then drawn in the explicit major mode
  Taken(kD3DDevice_DrawIndexedTessellatedVertices, __imp__sub_82230838, ctx, base);
}

REX_EXTERN(__imp__sub_82223560);
REX_HOOK_RAW(sub_82223560) {  // D3D_ClearF
  // becomes a clear record (kOpClear) over viewport and scissor
  Taken(kD3D_ClearF, __imp__sub_82223560, ctx, base);
}

REX_EXTERN(__imp__sub_8222D6C8);
REX_HOOK_RAW(sub_8222D6C8) {  // D3DDevice_Resolve
  // becomes a resolve record (kOpResolve) into the named D3DTexture
  Taken(kD3DDevice_Resolve, __imp__sub_8222D6C8, ctx, base);
}

REX_EXTERN(__imp__sub_82210408);
REX_HOOK_RAW(sub_82210408) {  // D3D_CDevice_AddCallsToPrimaryBuffer
  // the doorbell: publishes the list with the segments the ring is handed
  Taken(kD3D_CDevice_AddCallsToPrimaryBuffer, __imp__sub_82210408, ctx, base);
}

REX_EXTERN(__imp__sub_822052C8);
REX_HOOK_RAW(sub_822052C8) {  // D3D_InsertCallback
  // the callback's register writes and interrupt are recorded (kOpCallback); its PM4 is not walked
  Taken(kD3D_InsertCallback, __imp__sub_822052C8, ctx, base);
}

REX_EXTERN(__imp__sub_8228A748);
REX_HOOK_RAW(sub_8228A748) {  // D3D_SynchronizeToPresentationInterval
  // the present-interval wait is recorded (kOpCallback + kOpWaitMemory)
  Taken(kD3D_SynchronizeToPresentationInterval, __imp__sub_8228A748, ctx, base);
}

//=============================================================================
// Observed: the original runs, with reNut reading or adjusting around it
//=============================================================================

REX_EXTERN(__imp__sub_82657CB0);
REX_HOOK_RAW(sub_82657CB0) {  // D3DDevice_BeginCommandBuffer
  // a command buffer gets a host list of its own
  Observed(kD3DDevice_BeginCommandBuffer, __imp__sub_82657CB0, ctx, base);
}

REX_EXTERN(__imp__sub_826582A0);
REX_HOOK_RAW(sub_826582A0) {  // D3DDevice_EndCommandBuffer
  // flushes what is still pending into the buffer and closes its host list
  Observed(kD3DDevice_EndCommandBuffer, __imp__sub_826582A0, ctx, base);
}

REX_EXTERN(__imp__sub_82222888);
REX_HOOK_RAW(sub_82222888) {  // D3DDevice_RunCommandBuffer
  // the pending state the buffer inherits goes first, then its list is appended
  Observed(kD3DDevice_RunCommandBuffer, __imp__sub_82222888, ctx, base);
}

REX_EXTERN(__imp__sub_8222AA18);
REX_HOOK_RAW(sub_8222AA18) {  // D3DDevice_SetPredication
  // records SET_BIN_MASK_LO (0 means all) into the list
  Observed(kD3DDevice_SetPredication, __imp__sub_8222AA18, ctx, base);
}

REX_EXTERN(__imp__sub_82658678);
REX_HOOK_RAW(sub_82658678) {  // D3DDevice_SetCommandBufferPredication
  // records SET_BIN_MASK_LO/HI (0 means all) into the list
  Observed(kD3DDevice_SetCommandBufferPredication, __imp__sub_82658678, ctx, base);
}

REX_EXTERN(__imp__sub_82223270);
REX_HOOK_RAW(sub_82223270) {  // D3D_SetSurfaceClip
  // records the window scissor, which Direct3D writes straight into the ring
  Observed(kD3D_SetSurfaceClip, __imp__sub_82223270, ctx, base);
}

REX_EXTERN(__imp__sub_82287590);
REX_HOOK_RAW(sub_82287590) {  // D3DDevice_Swap
  // records the swap and publishes the immediate list
  Observed(kD3DDevice_Swap, __imp__sub_82287590, ctx, base);
}

REX_EXTERN(__imp__sub_82210AB0);
REX_HOOK_RAW(sub_82210AB0) {  // D3D_CDevice_KickOff
  // publishes the immediate list
  Observed(kD3D_CDevice_KickOff, __imp__sub_82210AB0, ctx, base);
}

REX_EXTERN(__imp__sub_822106C8);
REX_HOOK_RAW(sub_822106C8) {  // D3D_CDevice_KickOffSegment
  // publishes the immediate list
  Observed(kD3D_CDevice_KickOffSegment, __imp__sub_822106C8, ctx, base);
}

REX_EXTERN(__imp__sub_82278528);
REX_HOOK_RAW(sub_82278528) {  // D3D_CDevice_SetFence
  // records the fence write (kOpFence); the original still runs, its packets are never read
  Observed(kD3D_CDevice_SetFence, __imp__sub_82278528, ctx, base);
}

REX_EXTERN(__imp__sub_8264DCD0);
REX_HOOK_RAW(sub_8264DCD0) {  // D3D_CBlocker_Check
  // native-sync watchdog and the Plume handler's polling sleep; called tens of millions of times a second, so it is not counted
  Observed(kD3D_CBlocker_Check, __imp__sub_8264DCD0, ctx, base);
}

REX_EXTERN(__imp__sub_822788A0);
REX_HOOK_RAW(sub_822788A0) {  // D3DDevice_BeginTiling
  // with renut_single_tile, the bands are rewritten to one before the original runs
  Observed(kD3DDevice_BeginTiling, __imp__sub_822788A0, ctx, base);
}

REX_EXTERN(__imp__sub_822812C8);
REX_HOOK_RAW(sub_822812C8) {  // D3DDevice_Clear
  // the Plume handler mirrors the clear colour
  Observed(kD3DDevice_Clear, __imp__sub_822812C8, ctx, base);
}

REX_EXTERN(__imp__sub_822811A0);
REX_HOOK_RAW(sub_822811A0) {  // D3DDevice_ClearF
  // the Plume handler mirrors the clear colour
  Observed(kD3DDevice_ClearF, __imp__sub_822811A0, ctx, base);
}

REX_EXTERN(__imp__sub_8264E7B8);
REX_HOOK_RAW(sub_8264E7B8) {  // XGRegisterVertexShader
  // after the original, the shader container is dumped for offline recompilation
  Observed(kXGRegisterVertexShader, __imp__sub_8264E7B8, ctx, base);
}

REX_EXTERN(__imp__sub_8264E670);
REX_HOOK_RAW(sub_8264E670) {  // XGRegisterPixelShader
  // after the original, the shader container is dumped for offline recompilation
  Observed(kXGRegisterPixelShader, __imp__sub_8264E670, ctx, base);
}

REX_EXTERN(__imp__sub_8264E8B0);
REX_HOOK_RAW(sub_8264E8B0) {  // D3DDevice_CreateVertexShader
  // after the original, the shader container is dumped for offline recompilation
  Observed(kD3DDevice_CreateVertexShader, __imp__sub_8264E8B0, ctx, base);
}

REX_EXTERN(__imp__sub_8264E6A8);
REX_HOOK_RAW(sub_8264E6A8) {  // D3DDevice_CreatePixelShader
  // after the original, the shader container is dumped for offline recompilation
  Observed(kD3DDevice_CreatePixelShader, __imp__sub_8264E6A8, ctx, base);
}

//=============================================================================
// Forwarded: counted and probed, then the original runs
//=============================================================================

REX_EXTERN(__imp__sub_82655DA0);
REX_HOOK_RAW(sub_82655DA0) {  // D3DArrayTexture_GetArraySize
  Forwarded(kD3DArrayTexture_GetArraySize, __imp__sub_82655DA0, ctx, base);
}

REX_EXTERN(__imp__sub_82655DB0);
REX_HOOK_RAW(sub_82655DB0) {  // D3DArrayTexture_LockRect
  Forwarded(kD3DArrayTexture_LockRect, __imp__sub_82655DB0, ctx, base);
}

REX_EXTERN(__imp__sub_82655D88);
REX_HOOK_RAW(sub_82655D88) {  // D3DArrayTexture_UnlockRect
  Forwarded(kD3DArrayTexture_UnlockRect, __imp__sub_82655D88, ctx, base);
}

REX_EXTERN(__imp__sub_821EF890);
REX_HOOK_RAW(sub_821EF890) {  // D3DBaseTexture_GetLevelCount
  Forwarded(kD3DBaseTexture_GetLevelCount, __imp__sub_821EF890, ctx, base);
}

REX_EXTERN(__imp__sub_822693A0);
REX_HOOK_RAW(sub_822693A0) {  // D3DConstantBuffer_Unlock
  Forwarded(kD3DConstantBuffer_Unlock, __imp__sub_822693A0, ctx, base);
}

REX_EXTERN(__imp__sub_82655CD0);
REX_HOOK_RAW(sub_82655CD0) {  // D3DCubeTexture_GetCubeMapSurface
  Forwarded(kD3DCubeTexture_GetCubeMapSurface, __imp__sub_82655CD0, ctx, base);
}

REX_EXTERN(__imp__sub_8264E0D8);
REX_HOOK_RAW(sub_8264E0D8) {  // D3DDevice_AcquireThreadOwnership
  Forwarded(kD3DDevice_AcquireThreadOwnership, __imp__sub_8264E0D8, ctx, base);
}

REX_EXTERN(__imp__sub_8264E068);
REX_HOOK_RAW(sub_8264E068) {  // D3DDevice_AddRef
  Forwarded(kD3DDevice_AddRef, __imp__sub_8264E068, ctx, base);
}

REX_EXTERN(__imp__sub_82269618);
REX_HOOK_RAW(sub_82269618) {  // D3DDevice_BeginExport
  Forwarded(kD3DDevice_BeginExport, __imp__sub_82269618, ctx, base);
}

REX_EXTERN(__imp__sub_8228F3B0);
REX_HOOK_RAW(sub_8228F3B0) {  // D3DDevice_BlockOnFence
  Forwarded(kD3DDevice_BlockOnFence, __imp__sub_8228F3B0, ctx, base);
}

REX_EXTERN(__imp__sub_82654EE0);
REX_HOOK_RAW(sub_82654EE0) {  // D3DDevice_CreateIndexBuffer
  Forwarded(kD3DDevice_CreateIndexBuffer, __imp__sub_82654EE0, ctx, base);
}

REX_EXTERN(__imp__sub_82655FE8);
REX_HOOK_RAW(sub_82655FE8) {  // D3DDevice_CreateSurface
  Forwarded(kD3DDevice_CreateSurface, __imp__sub_82655FE8, ctx, base);
}

REX_EXTERN(__imp__sub_82654DB8);
REX_HOOK_RAW(sub_82654DB8) {  // D3DDevice_CreateVertexBuffer
  Forwarded(kD3DDevice_CreateVertexBuffer, __imp__sub_82654DB8, ctx, base);
}

REX_EXTERN(__imp__sub_8264EA90);
REX_HOOK_RAW(sub_8264EA90) {  // D3DDevice_CreateVertexDeclaration
  Forwarded(kD3DDevice_CreateVertexDeclaration, __imp__sub_8264EA90, ctx, base);
}

REX_EXTERN(__imp__sub_82656728);
REX_HOOK_RAW(sub_82656728) {  // D3DDevice_DrawIndexedVerticesUP
  Forwarded(kD3DDevice_DrawIndexedVerticesUP, __imp__sub_82656728, ctx, base);
}

REX_EXTERN(__imp__sub_8222E520);
REX_HOOK_RAW(sub_8222E520) {  // D3DDevice_DrawVerticesUP
  Forwarded(kD3DDevice_DrawVerticesUP, __imp__sub_8222E520, ctx, base);
}

REX_EXTERN(__imp__sub_822693B0);
REX_HOOK_RAW(sub_822693B0) {  // D3DDevice_EndExport
  Forwarded(kD3DDevice_EndExport, __imp__sub_822693B0, ctx, base);
}

REX_EXTERN(__imp__sub_826561F8);
REX_HOOK_RAW(sub_826561F8) {  // D3DDevice_EndIndexedVertices
  Forwarded(kD3DDevice_EndIndexedVertices, __imp__sub_826561F8, ctx, base);
}

REX_EXTERN(__imp__sub_82212328);
REX_HOOK_RAW(sub_82212328) {  // D3DDevice_EndTiling
  Forwarded(kD3DDevice_EndTiling, __imp__sub_82212328, ctx, base);
}

REX_EXTERN(__imp__sub_8264E128);
REX_HOOK_RAW(sub_8264E128) {  // D3DDevice_FlushHiZStencil
  Forwarded(kD3DDevice_FlushHiZStencil, __imp__sub_8264E128, ctx, base);
}

REX_EXTERN(__imp__sub_8264D910);
REX_HOOK_RAW(sub_8264D910) {  // D3DDevice_GetDepthStencilSurface
  Forwarded(kD3DDevice_GetDepthStencilSurface, __imp__sub_8264D910, ctx, base);
}

REX_EXTERN(__imp__sub_82658948);
REX_HOOK_RAW(sub_82658948) {  // D3DDevice_GetDeviceCaps
  Forwarded(kD3DDevice_GetDeviceCaps, __imp__sub_82658948, ctx, base);
}

REX_EXTERN(__imp__sub_82658960);
REX_HOOK_RAW(sub_82658960) {  // D3DDevice_GetDisplayMode
  Forwarded(kD3DDevice_GetDisplayMode, __imp__sub_82658960, ctx, base);
}

REX_EXTERN(__imp__sub_8264D8D0);
REX_HOOK_RAW(sub_8264D8D0) {  // D3DDevice_GetRenderTarget
  Forwarded(kD3DDevice_GetRenderTarget, __imp__sub_8264D8D0, ctx, base);
}

REX_EXTERN(__imp__sub_8264D840);
REX_HOOK_RAW(sub_8264D840) {  // D3DDevice_GetStreamSource
  Forwarded(kD3DDevice_GetStreamSource, __imp__sub_8264D840, ctx, base);
}

REX_EXTERN(__imp__sub_82276AC0);
REX_HOOK_RAW(sub_82276AC0) {  // D3DDevice_GetViewport
  Forwarded(kD3DDevice_GetViewport, __imp__sub_82276AC0, ctx, base);
}

REX_EXTERN(__imp__sub_821FB688);
REX_HOOK_RAW(sub_821FB688) {  // D3DDevice_InsertCallback
  Forwarded(kD3DDevice_InsertCallback, __imp__sub_821FB688, ctx, base);
}

REX_EXTERN(__imp__sub_8228F650);
REX_HOOK_RAW(sub_8228F650) {  // D3DDevice_IsFencePending
  Forwarded(kD3DDevice_IsFencePending, __imp__sub_8228F650, ctx, base);
}

REX_EXTERN(__imp__sub_82654778);
REX_HOOK_RAW(sub_82654778) {  // D3DDevice_Present
  Forwarded(kD3DDevice_Present, __imp__sub_82654778, ctx, base);
}

REX_EXTERN(__imp__sub_8264E118);
REX_HOOK_RAW(sub_8264E118) {  // D3DDevice_ReleaseThreadOwnership
  Forwarded(kD3DDevice_ReleaseThreadOwnership, __imp__sub_8264E118, ctx, base);
}

REX_EXTERN(__imp__sub_8264DAE8);
REX_HOOK_RAW(sub_8264DAE8) {  // D3DDevice_SetClipPlane
  Forwarded(kD3DDevice_SetClipPlane, __imp__sub_8264DAE8, ctx, base);
}

REX_EXTERN(__imp__sub_8227AF40);
REX_HOOK_RAW(sub_8227AF40) {  // D3DDevice_SetDepthStencilSurface
  Forwarded(kD3DDevice_SetDepthStencilSurface, __imp__sub_8227AF40, ctx, base);
}

REX_EXTERN(__imp__sub_8264D948);
REX_HOOK_RAW(sub_8264D948) {  // D3DDevice_SetGammaRamp
  Forwarded(kD3DDevice_SetGammaRamp, __imp__sub_8264D948, ctx, base);
}

REX_EXTERN(__imp__sub_8220EE38);
REX_HOOK_RAW(sub_8220EE38) {  // D3DDevice_SetIndices
  Forwarded(kD3DDevice_SetIndices, __imp__sub_8220EE38, ctx, base);
}

REX_EXTERN(__imp__sub_8264DA18);
REX_HOOK_RAW(sub_8264DA18) {  // D3DDevice_SetPWLGamma
  Forwarded(kD3DDevice_SetPWLGamma, __imp__sub_8264DA18, ctx, base);
}

REX_EXTERN(__imp__sub_82226730);
REX_HOOK_RAW(sub_82226730) {  // D3DDevice_SetPixelShader
  Forwarded(kD3DDevice_SetPixelShader, __imp__sub_82226730, ctx, base);
}

REX_EXTERN(__imp__sub_82268810);
REX_HOOK_RAW(sub_82268810) {  // D3DDevice_SetPixelShaderConstantB
  Forwarded(kD3DDevice_SetPixelShaderConstantB, __imp__sub_82268810, ctx, base);
}

REX_EXTERN(__imp__sub_8222F0D8);
REX_HOOK_RAW(sub_8222F0D8) {  // D3DDevice_SetPixelShaderConstantFN
  Forwarded(kD3DDevice_SetPixelShaderConstantFN, __imp__sub_8222F0D8, ctx, base);
}

REX_EXTERN(__imp__sub_8264E5C8);
REX_HOOK_RAW(sub_8264E5C8) {  // D3DDevice_SetPixelShaderConstantI
  Forwarded(kD3DDevice_SetPixelShaderConstantI, __imp__sub_8264E5C8, ctx, base);
}

REX_EXTERN(__imp__sub_8222A718);
REX_HOOK_RAW(sub_8222A718) {  // D3DDevice_SetRenderState_AlphaBlendEnable
  Forwarded(kD3DDevice_SetRenderState_AlphaBlendEnable, __imp__sub_8222A718, ctx, base);
}

REX_EXTERN(__imp__sub_8264BFB8);
REX_HOOK_RAW(sub_8264BFB8) {  // D3DDevice_SetRenderState_AlphaFunc
  Forwarded(kD3DDevice_SetRenderState_AlphaFunc, __imp__sub_8264BFB8, ctx, base);
}

REX_EXTERN(__imp__sub_8264BF50);
REX_HOOK_RAW(sub_8264BF50) {  // D3DDevice_SetRenderState_AlphaRef
  Forwarded(kD3DDevice_SetRenderState_AlphaRef, __imp__sub_8264BF50, ctx, base);
}

REX_EXTERN(__imp__sub_8264BE98);
REX_HOOK_RAW(sub_8264BE98) {  // D3DDevice_SetRenderState_AlphaTestEnable
  Forwarded(kD3DDevice_SetRenderState_AlphaTestEnable, __imp__sub_8264BE98, ctx, base);
}

REX_EXTERN(__imp__sub_8264CE88);
REX_HOOK_RAW(sub_8264CE88) {  // D3DDevice_SetRenderState_AlphaToMaskEnable
  Forwarded(kD3DDevice_SetRenderState_AlphaToMaskEnable, __imp__sub_8264CE88, ctx, base);
}

REX_EXTERN(__imp__sub_8264CEB8);
REX_HOOK_RAW(sub_8264CEB8) {  // D3DDevice_SetRenderState_AlphaToMaskOffsets
  Forwarded(kD3DDevice_SetRenderState_AlphaToMaskOffsets, __imp__sub_8264CEB8, ctx, base);
}

REX_EXTERN(__imp__sub_8226E8E0);
REX_HOOK_RAW(sub_8226E8E0) {  // D3DDevice_SetRenderState_BlendFactor
  Forwarded(kD3DDevice_SetRenderState_BlendFactor, __imp__sub_8226E8E0, ctx, base);
}

REX_EXTERN(__imp__sub_82226380);
REX_HOOK_RAW(sub_82226380) {  // D3DDevice_SetRenderState_BlendOp
  Forwarded(kD3DDevice_SetRenderState_BlendOp, __imp__sub_82226380, ctx, base);
}

REX_EXTERN(__imp__sub_8221F5F8);
REX_HOOK_RAW(sub_8221F5F8) {  // D3DDevice_SetRenderState_BlendOpAlpha
  Forwarded(kD3DDevice_SetRenderState_BlendOpAlpha, __imp__sub_8221F5F8, ctx, base);
}

REX_EXTERN(__imp__sub_8264C110);
REX_HOOK_RAW(sub_8264C110) {  // D3DDevice_SetRenderState_Buffer2Frames
  Forwarded(kD3DDevice_SetRenderState_Buffer2Frames, __imp__sub_8264C110, ctx, base);
}

REX_EXTERN(__imp__sub_8264C338);
REX_HOOK_RAW(sub_8264C338) {  // D3DDevice_SetRenderState_CCWStencilFail
  Forwarded(kD3DDevice_SetRenderState_CCWStencilFail, __imp__sub_8264C338, ctx, base);
}

REX_EXTERN(__imp__sub_8264C308);
REX_HOOK_RAW(sub_8264C308) {  // D3DDevice_SetRenderState_CCWStencilFunc
  Forwarded(kD3DDevice_SetRenderState_CCWStencilFunc, __imp__sub_8264C308, ctx, base);
}

REX_EXTERN(__imp__sub_8264C458);
REX_HOOK_RAW(sub_8264C458) {  // D3DDevice_SetRenderState_CCWStencilMask
  Forwarded(kD3DDevice_SetRenderState_CCWStencilMask, __imp__sub_8264C458, ctx, base);
}

REX_EXTERN(__imp__sub_8264C3A8);
REX_HOOK_RAW(sub_8264C3A8) {  // D3DDevice_SetRenderState_CCWStencilPass
  Forwarded(kD3DDevice_SetRenderState_CCWStencilPass, __imp__sub_8264C3A8, ctx, base);
}

REX_EXTERN(__imp__sub_8264C438);
REX_HOOK_RAW(sub_8264C438) {  // D3DDevice_SetRenderState_CCWStencilRef
  Forwarded(kD3DDevice_SetRenderState_CCWStencilRef, __imp__sub_8264C438, ctx, base);
}

REX_EXTERN(__imp__sub_8264C478);
REX_HOOK_RAW(sub_8264C478) {  // D3DDevice_SetRenderState_CCWStencilWriteMask
  Forwarded(kD3DDevice_SetRenderState_CCWStencilWriteMask, __imp__sub_8264C478, ctx, base);
}

REX_EXTERN(__imp__sub_8264C370);
REX_HOOK_RAW(sub_8264C370) {  // D3DDevice_SetRenderState_CCWStencilZFail
  Forwarded(kD3DDevice_SetRenderState_CCWStencilZFail, __imp__sub_8264C370, ctx, base);
}

REX_EXTERN(__imp__sub_8228F370);
REX_HOOK_RAW(sub_8228F370) {  // D3DDevice_SetRenderState_ClipPlaneEnable
  Forwarded(kD3DDevice_SetRenderState_ClipPlaneEnable, __imp__sub_8228F370, ctx, base);
}

REX_EXTERN(__imp__sub_821FB6E0);
REX_HOOK_RAW(sub_821FB6E0) {  // D3DDevice_SetRenderState_ColorWriteEnable
  Forwarded(kD3DDevice_SetRenderState_ColorWriteEnable, __imp__sub_821FB6E0, ctx, base);
}

REX_EXTERN(__imp__sub_8264C5F0);
REX_HOOK_RAW(sub_8264C5F0) {  // D3DDevice_SetRenderState_ColorWriteEnable1
  Forwarded(kD3DDevice_SetRenderState_ColorWriteEnable1, __imp__sub_8264C5F0, ctx, base);
}

REX_EXTERN(__imp__sub_8264C630);
REX_HOOK_RAW(sub_8264C630) {  // D3DDevice_SetRenderState_ColorWriteEnable2
  Forwarded(kD3DDevice_SetRenderState_ColorWriteEnable2, __imp__sub_8264C630, ctx, base);
}

REX_EXTERN(__imp__sub_8264C670);
REX_HOOK_RAW(sub_8264C670) {  // D3DDevice_SetRenderState_ColorWriteEnable3
  Forwarded(kD3DDevice_SetRenderState_ColorWriteEnable3, __imp__sub_8264C670, ctx, base);
}

REX_EXTERN(__imp__sub_8228F350);
REX_HOOK_RAW(sub_8228F350) {  // D3DDevice_SetRenderState_CullMode
  Forwarded(kD3DDevice_SetRenderState_CullMode, __imp__sub_8228F350, ctx, base);
}

REX_EXTERN(__imp__sub_821FB718);
REX_HOOK_RAW(sub_821FB718) {  // D3DDevice_SetRenderState_DepthBias
  Forwarded(kD3DDevice_SetRenderState_DepthBias, __imp__sub_821FB718, ctx, base);
}

REX_EXTERN(__imp__sub_822307B8);
REX_HOOK_RAW(sub_822307B8) {  // D3DDevice_SetRenderState_DestBlend
  Forwarded(kD3DDevice_SetRenderState_DestBlend, __imp__sub_822307B8, ctx, base);
}

REX_EXTERN(__imp__sub_822208E8);
REX_HOOK_RAW(sub_822208E8) {  // D3DDevice_SetRenderState_DestBlendAlpha
  Forwarded(kD3DDevice_SetRenderState_DestBlendAlpha, __imp__sub_822208E8, ctx, base);
}

REX_EXTERN(__imp__sub_8264CF50);
REX_HOOK_RAW(sub_8264CF50) {  // D3DDevice_SetRenderState_DiscardBand_X
  Forwarded(kD3DDevice_SetRenderState_DiscardBand_X, __imp__sub_8264CF50, ctx, base);
}

REX_EXTERN(__imp__sub_8264CF78);
REX_HOOK_RAW(sub_8264CF78) {  // D3DDevice_SetRenderState_DiscardBand_Y
  Forwarded(kD3DDevice_SetRenderState_DiscardBand_Y, __imp__sub_8264CF78, ctx, base);
}

REX_EXTERN(__imp__sub_8228F330);
REX_HOOK_RAW(sub_8228F330) {  // D3DDevice_SetRenderState_FillMode
  Forwarded(kD3DDevice_SetRenderState_FillMode, __imp__sub_8228F330, ctx, base);
}

REX_EXTERN(__imp__sub_8264CEF0);
REX_HOOK_RAW(sub_8264CEF0) {  // D3DDevice_SetRenderState_GuardBand_X
  Forwarded(kD3DDevice_SetRenderState_GuardBand_X, __imp__sub_8264CEF0, ctx, base);
}

REX_EXTERN(__imp__sub_8264CF20);
REX_HOOK_RAW(sub_8264CF20) {  // D3DDevice_SetRenderState_GuardBand_Y
  Forwarded(kD3DDevice_SetRenderState_GuardBand_Y, __imp__sub_8264CF20, ctx, base);
}

REX_EXTERN(__imp__sub_8264CDF8);
REX_HOOK_RAW(sub_8264CDF8) {  // D3DDevice_SetRenderState_HalfPixelOffset
  Forwarded(kD3DDevice_SetRenderState_HalfPixelOffset, __imp__sub_8264CDF8, ctx, base);
}

REX_EXTERN(__imp__sub_8264CFE8);
REX_HOOK_RAW(sub_8264CFE8) {  // D3DDevice_SetRenderState_HiStencilEnable
  Forwarded(kD3DDevice_SetRenderState_HiStencilEnable, __imp__sub_8264CFE8, ctx, base);
}

REX_EXTERN(__imp__sub_8264D048);
REX_HOOK_RAW(sub_8264D048) {  // D3DDevice_SetRenderState_HiStencilFunc
  Forwarded(kD3DDevice_SetRenderState_HiStencilFunc, __imp__sub_8264D048, ctx, base);
}

REX_EXTERN(__imp__sub_8264D078);
REX_HOOK_RAW(sub_8264D078) {  // D3DDevice_SetRenderState_HiStencilRef
  Forwarded(kD3DDevice_SetRenderState_HiStencilRef, __imp__sub_8264D078, ctx, base);
}

REX_EXTERN(__imp__sub_8264D018);
REX_HOOK_RAW(sub_8264D018) {  // D3DDevice_SetRenderState_HiStencilWriteEnable
  Forwarded(kD3DDevice_SetRenderState_HiStencilWriteEnable, __imp__sub_8264D018, ctx, base);
}

REX_EXTERN(__imp__sub_8264CFA8);
REX_HOOK_RAW(sub_8264CFA8) {  // D3DDevice_SetRenderState_HiZEnable
  Forwarded(kD3DDevice_SetRenderState_HiZEnable, __imp__sub_8264CFA8, ctx, base);
}

REX_EXTERN(__imp__sub_822899D0);
REX_HOOK_RAW(sub_822899D0) {  // D3DDevice_SetRenderState_HiZWriteEnable
  Forwarded(kD3DDevice_SetRenderState_HiZWriteEnable, __imp__sub_822899D0, ctx, base);
}

REX_EXTERN(__imp__sub_8264CAF0);
REX_HOOK_RAW(sub_8264CAF0) {  // D3DDevice_SetRenderState_HighPrecisionBlendEnable
  Forwarded(kD3DDevice_SetRenderState_HighPrecisionBlendEnable, __imp__sub_8264CAF0, ctx, base);
}

REX_EXTERN(__imp__sub_8264CB88);
REX_HOOK_RAW(sub_8264CB88) {  // D3DDevice_SetRenderState_HighPrecisionBlendEnable1
  Forwarded(kD3DDevice_SetRenderState_HighPrecisionBlendEnable1, __imp__sub_8264CB88, ctx, base);
}

REX_EXTERN(__imp__sub_8264CC20);
REX_HOOK_RAW(sub_8264CC20) {  // D3DDevice_SetRenderState_HighPrecisionBlendEnable2
  Forwarded(kD3DDevice_SetRenderState_HighPrecisionBlendEnable2, __imp__sub_8264CC20, ctx, base);
}

REX_EXTERN(__imp__sub_8264CCB8);
REX_HOOK_RAW(sub_8264CCB8) {  // D3DDevice_SetRenderState_HighPrecisionBlendEnable3
  Forwarded(kD3DDevice_SetRenderState_HighPrecisionBlendEnable3, __imp__sub_8264CCB8, ctx, base);
}

REX_EXTERN(__imp__sub_8264C060);
REX_HOOK_RAW(sub_8264C060) {  // D3DDevice_SetRenderState_LastPixel
  Forwarded(kD3DDevice_SetRenderState_LastPixel, __imp__sub_8264C060, ctx, base);
}

REX_EXTERN(__imp__sub_8264C098);
REX_HOOK_RAW(sub_8264C098) {  // D3DDevice_SetRenderState_LineWidth
  Forwarded(kD3DDevice_SetRenderState_LineWidth, __imp__sub_8264C098, ctx, base);
}

REX_EXTERN(__imp__sub_8264CD88);
REX_HOOK_RAW(sub_8264CD88) {  // D3DDevice_SetRenderState_MaxTessellationLevel
  Forwarded(kD3DDevice_SetRenderState_MaxTessellationLevel, __imp__sub_8264CD88, ctx, base);
}

REX_EXTERN(__imp__sub_8264CD50);
REX_HOOK_RAW(sub_8264CD50) {  // D3DDevice_SetRenderState_MinTessellationLevel
  Forwarded(kD3DDevice_SetRenderState_MinTessellationLevel, __imp__sub_8264CD50, ctx, base);
}

REX_EXTERN(__imp__sub_8264C598);
REX_HOOK_RAW(sub_8264C598) {  // D3DDevice_SetRenderState_MultiSampleAntiAlias
  Forwarded(kD3DDevice_SetRenderState_MultiSampleAntiAlias, __imp__sub_8264C598, ctx, base);
}

REX_EXTERN(__imp__sub_8264C5C8);
REX_HOOK_RAW(sub_8264C5C8) {  // D3DDevice_SetRenderState_MultiSampleMask
  Forwarded(kD3DDevice_SetRenderState_MultiSampleMask, __imp__sub_8264C5C8, ctx, base);
}

REX_EXTERN(__imp__sub_8264C6C0);
REX_HOOK_RAW(sub_8264C6C0) {  // D3DDevice_SetRenderState_PointSize
  Forwarded(kD3DDevice_SetRenderState_PointSize, __imp__sub_8264C6C0, ctx, base);
}

REX_EXTERN(__imp__sub_822687D0);
REX_HOOK_RAW(sub_822687D0) {  // D3DDevice_SetRenderState_PointSizeMax
  Forwarded(kD3DDevice_SetRenderState_PointSizeMax, __imp__sub_822687D0, ctx, base);
}

REX_EXTERN(__imp__sub_8264C720);
REX_HOOK_RAW(sub_8264C720) {  // D3DDevice_SetRenderState_PointSizeMin
  Forwarded(kD3DDevice_SetRenderState_PointSizeMin, __imp__sub_8264C720, ctx, base);
}

REX_EXTERN(__imp__sub_8264C6B0);
REX_HOOK_RAW(sub_8264C6B0) {  // D3DDevice_SetRenderState_PointSpriteEnable
  Forwarded(kD3DDevice_SetRenderState_PointSpriteEnable, __imp__sub_8264C6B0, ctx, base);
}

REX_EXTERN(__imp__sub_8264D0A8);
REX_HOOK_RAW(sub_8264D0A8) {  // D3DDevice_SetRenderState_PresentImmediateThreshold
  Forwarded(kD3DDevice_SetRenderState_PresentImmediateThreshold, __imp__sub_8264D0A8, ctx, base);
}

REX_EXTERN(__imp__sub_8264D098);
REX_HOOK_RAW(sub_8264D098) {  // D3DDevice_SetRenderState_PresentInterval
  Forwarded(kD3DDevice_SetRenderState_PresentInterval, __imp__sub_8264D098, ctx, base);
}

REX_EXTERN(__imp__sub_8264CE30);
REX_HOOK_RAW(sub_8264CE30) {  // D3DDevice_SetRenderState_PrimitiveResetEnable
  Forwarded(kD3DDevice_SetRenderState_PrimitiveResetEnable, __imp__sub_8264CE30, ctx, base);
}

REX_EXTERN(__imp__sub_8264CE60);
REX_HOOK_RAW(sub_8264CE60) {  // D3DDevice_SetRenderState_PrimitiveResetIndex
  Forwarded(kD3DDevice_SetRenderState_PrimitiveResetIndex, __imp__sub_8264CE60, ctx, base);
}

REX_EXTERN(__imp__sub_8264C4A8);
REX_HOOK_RAW(sub_8264C4A8) {  // D3DDevice_SetRenderState_ScissorTestEnable
  Forwarded(kD3DDevice_SetRenderState_ScissorTestEnable, __imp__sub_8264C4A8, ctx, base);
}

REX_EXTERN(__imp__sub_8222A690);
REX_HOOK_RAW(sub_8222A690) {  // D3DDevice_SetRenderState_SeparateAlphaBlendEnable
  Forwarded(kD3DDevice_SetRenderState_SeparateAlphaBlendEnable, __imp__sub_8222A690, ctx, base);
}

REX_EXTERN(__imp__sub_8264C4C0);
REX_HOOK_RAW(sub_8264C4C0) {  // D3DDevice_SetRenderState_SlopeScaleDepthBias
  Forwarded(kD3DDevice_SetRenderState_SlopeScaleDepthBias, __imp__sub_8264C4C0, ctx, base);
}

REX_EXTERN(__imp__sub_82230090);
REX_HOOK_RAW(sub_82230090) {  // D3DDevice_SetRenderState_SrcBlend
  Forwarded(kD3DDevice_SetRenderState_SrcBlend, __imp__sub_82230090, ctx, base);
}

REX_EXTERN(__imp__sub_822672B8);
REX_HOOK_RAW(sub_822672B8) {  // D3DDevice_SetRenderState_SrcBlendAlpha
  Forwarded(kD3DDevice_SetRenderState_SrcBlendAlpha, __imp__sub_822672B8, ctx, base);
}

REX_EXTERN(__imp__sub_8264C1C0);
REX_HOOK_RAW(sub_8264C1C0) {  // D3DDevice_SetRenderState_StencilEnable
  Forwarded(kD3DDevice_SetRenderState_StencilEnable, __imp__sub_8264C1C0, ctx, base);
}

REX_EXTERN(__imp__sub_8264C268);
REX_HOOK_RAW(sub_8264C268) {  // D3DDevice_SetRenderState_StencilFail
  Forwarded(kD3DDevice_SetRenderState_StencilFail, __imp__sub_8264C268, ctx, base);
}

REX_EXTERN(__imp__sub_8264C238);
REX_HOOK_RAW(sub_8264C238) {  // D3DDevice_SetRenderState_StencilFunc
  Forwarded(kD3DDevice_SetRenderState_StencilFunc, __imp__sub_8264C238, ctx, base);
}

REX_EXTERN(__imp__sub_8264C3F8);
REX_HOOK_RAW(sub_8264C3F8) {  // D3DDevice_SetRenderState_StencilMask
  Forwarded(kD3DDevice_SetRenderState_StencilMask, __imp__sub_8264C3F8, ctx, base);
}

REX_EXTERN(__imp__sub_8264C2D8);
REX_HOOK_RAW(sub_8264C2D8) {  // D3DDevice_SetRenderState_StencilPass
  Forwarded(kD3DDevice_SetRenderState_StencilPass, __imp__sub_8264C2D8, ctx, base);
}

REX_EXTERN(__imp__sub_8264C3D8);
REX_HOOK_RAW(sub_8264C3D8) {  // D3DDevice_SetRenderState_StencilRef
  Forwarded(kD3DDevice_SetRenderState_StencilRef, __imp__sub_8264C3D8, ctx, base);
}

REX_EXTERN(__imp__sub_8264C418);
REX_HOOK_RAW(sub_8264C418) {  // D3DDevice_SetRenderState_StencilWriteMask
  Forwarded(kD3DDevice_SetRenderState_StencilWriteMask, __imp__sub_8264C418, ctx, base);
}

REX_EXTERN(__imp__sub_8264C2A0);
REX_HOOK_RAW(sub_8264C2A0) {  // D3DDevice_SetRenderState_StencilZFail
  Forwarded(kD3DDevice_SetRenderState_StencilZFail, __imp__sub_8264C2A0, ctx, base);
}

REX_EXTERN(__imp__sub_8264CDC0);
REX_HOOK_RAW(sub_8264CDC0) {  // D3DDevice_SetRenderState_TessellationMode
  Forwarded(kD3DDevice_SetRenderState_TessellationMode, __imp__sub_8264CDC0, ctx, base);
}

REX_EXTERN(__imp__sub_8264C200);
REX_HOOK_RAW(sub_8264C200) {  // D3DDevice_SetRenderState_TwoSidedStencilMode
  Forwarded(kD3DDevice_SetRenderState_TwoSidedStencilMode, __imp__sub_8264C200, ctx, base);
}

REX_EXTERN(__imp__sub_82205A80);
REX_HOOK_RAW(sub_82205A80) {  // D3DDevice_SetRenderState_ViewportEnable
  Forwarded(kD3DDevice_SetRenderState_ViewportEnable, __imp__sub_82205A80, ctx, base);
}

REX_EXTERN(__imp__sub_8264C780);
REX_HOOK_RAW(sub_8264C780) {  // D3DDevice_SetRenderState_Wrap0
  Forwarded(kD3DDevice_SetRenderState_Wrap0, __imp__sub_8264C780, ctx, base);
}

REX_EXTERN(__imp__sub_8264C7A0);
REX_HOOK_RAW(sub_8264C7A0) {  // D3DDevice_SetRenderState_Wrap1
  Forwarded(kD3DDevice_SetRenderState_Wrap1, __imp__sub_8264C7A0, ctx, base);
}

REX_EXTERN(__imp__sub_8264C8F8);
REX_HOOK_RAW(sub_8264C8F8) {  // D3DDevice_SetRenderState_Wrap10
  Forwarded(kD3DDevice_SetRenderState_Wrap10, __imp__sub_8264C8F8, ctx, base);
}

REX_EXTERN(__imp__sub_8264C920);
REX_HOOK_RAW(sub_8264C920) {  // D3DDevice_SetRenderState_Wrap11
  Forwarded(kD3DDevice_SetRenderState_Wrap11, __imp__sub_8264C920, ctx, base);
}

REX_EXTERN(__imp__sub_8264C948);
REX_HOOK_RAW(sub_8264C948) {  // D3DDevice_SetRenderState_Wrap12
  Forwarded(kD3DDevice_SetRenderState_Wrap12, __imp__sub_8264C948, ctx, base);
}

REX_EXTERN(__imp__sub_8264C970);
REX_HOOK_RAW(sub_8264C970) {  // D3DDevice_SetRenderState_Wrap13
  Forwarded(kD3DDevice_SetRenderState_Wrap13, __imp__sub_8264C970, ctx, base);
}

REX_EXTERN(__imp__sub_8264C998);
REX_HOOK_RAW(sub_8264C998) {  // D3DDevice_SetRenderState_Wrap14
  Forwarded(kD3DDevice_SetRenderState_Wrap14, __imp__sub_8264C998, ctx, base);
}

REX_EXTERN(__imp__sub_8264C9C0);
REX_HOOK_RAW(sub_8264C9C0) {  // D3DDevice_SetRenderState_Wrap15
  Forwarded(kD3DDevice_SetRenderState_Wrap15, __imp__sub_8264C9C0, ctx, base);
}

REX_EXTERN(__imp__sub_8264C7C8);
REX_HOOK_RAW(sub_8264C7C8) {  // D3DDevice_SetRenderState_Wrap2
  Forwarded(kD3DDevice_SetRenderState_Wrap2, __imp__sub_8264C7C8, ctx, base);
}

REX_EXTERN(__imp__sub_8264C7F0);
REX_HOOK_RAW(sub_8264C7F0) {  // D3DDevice_SetRenderState_Wrap3
  Forwarded(kD3DDevice_SetRenderState_Wrap3, __imp__sub_8264C7F0, ctx, base);
}

REX_EXTERN(__imp__sub_8264C818);
REX_HOOK_RAW(sub_8264C818) {  // D3DDevice_SetRenderState_Wrap4
  Forwarded(kD3DDevice_SetRenderState_Wrap4, __imp__sub_8264C818, ctx, base);
}

REX_EXTERN(__imp__sub_8264C840);
REX_HOOK_RAW(sub_8264C840) {  // D3DDevice_SetRenderState_Wrap5
  Forwarded(kD3DDevice_SetRenderState_Wrap5, __imp__sub_8264C840, ctx, base);
}

REX_EXTERN(__imp__sub_8264C868);
REX_HOOK_RAW(sub_8264C868) {  // D3DDevice_SetRenderState_Wrap6
  Forwarded(kD3DDevice_SetRenderState_Wrap6, __imp__sub_8264C868, ctx, base);
}

REX_EXTERN(__imp__sub_8264C890);
REX_HOOK_RAW(sub_8264C890) {  // D3DDevice_SetRenderState_Wrap7
  Forwarded(kD3DDevice_SetRenderState_Wrap7, __imp__sub_8264C890, ctx, base);
}

REX_EXTERN(__imp__sub_8264C8B0);
REX_HOOK_RAW(sub_8264C8B0) {  // D3DDevice_SetRenderState_Wrap8
  Forwarded(kD3DDevice_SetRenderState_Wrap8, __imp__sub_8264C8B0, ctx, base);
}

REX_EXTERN(__imp__sub_8264C8D0);
REX_HOOK_RAW(sub_8264C8D0) {  // D3DDevice_SetRenderState_Wrap9
  Forwarded(kD3DDevice_SetRenderState_Wrap9, __imp__sub_8264C8D0, ctx, base);
}

REX_EXTERN(__imp__sub_8264C140);
REX_HOOK_RAW(sub_8264C140) {  // D3DDevice_SetRenderState_ZEnable
  Forwarded(kD3DDevice_SetRenderState_ZEnable, __imp__sub_8264C140, ctx, base);
}

REX_EXTERN(__imp__sub_821FB7B0);
REX_HOOK_RAW(sub_821FB7B0) {  // D3DDevice_SetRenderState_ZFunc
  Forwarded(kD3DDevice_SetRenderState_ZFunc, __imp__sub_821FB7B0, ctx, base);
}

REX_EXTERN(__imp__sub_8264C180);
REX_HOOK_RAW(sub_8264C180) {  // D3DDevice_SetRenderState_ZWriteEnable
  Forwarded(kD3DDevice_SetRenderState_ZWriteEnable, __imp__sub_8264C180, ctx, base);
}

REX_EXTERN(__imp__sub_8227AB68);
REX_HOOK_RAW(sub_8227AB68) {  // D3DDevice_SetRenderTarget
  Forwarded(kD3DDevice_SetRenderTarget, __imp__sub_8227AB68, ctx, base);
}

REX_EXTERN(__imp__sub_82657508);
REX_HOOK_RAW(sub_82657508) {  // D3DDevice_SetRingBufferParameters
  Forwarded(kD3DDevice_SetRingBufferParameters, __imp__sub_82657508, ctx, base);
}

REX_EXTERN(__imp__sub_8227D2D0);
REX_HOOK_RAW(sub_8227D2D0) {  // D3DDevice_SetSamplerState_AddressU
  Forwarded(kD3DDevice_SetSamplerState_AddressU, __imp__sub_8227D2D0, ctx, base);
}

REX_EXTERN(__imp__sub_8220C4E0);
REX_HOOK_RAW(sub_8220C4E0) {  // D3DDevice_SetSamplerState_AddressV
  Forwarded(kD3DDevice_SetSamplerState_AddressV, __imp__sub_8220C4E0, ctx, base);
}

REX_EXTERN(__imp__sub_8264D630);
REX_HOOK_RAW(sub_8264D630) {  // D3DDevice_SetSamplerState_AddressW
  Forwarded(kD3DDevice_SetSamplerState_AddressW, __imp__sub_8264D630, ctx, base);
}

REX_EXTERN(__imp__sub_8264D3B8);
REX_HOOK_RAW(sub_8264D3B8) {  // D3DDevice_SetSamplerState_AnisotropyBias
  Forwarded(kD3DDevice_SetSamplerState_AnisotropyBias, __imp__sub_8264D3B8, ctx, base);
}

REX_EXTERN(__imp__sub_8264D590);
REX_HOOK_RAW(sub_8264D590) {  // D3DDevice_SetSamplerState_BorderColor
  Forwarded(kD3DDevice_SetSamplerState_BorderColor, __imp__sub_8264D590, ctx, base);
}

REX_EXTERN(__imp__sub_8264D6D8);
REX_HOOK_RAW(sub_8264D6D8) {  // D3DDevice_SetSamplerState_HGradientExpBias
  Forwarded(kD3DDevice_SetSamplerState_HGradientExpBias, __imp__sub_8264D6D8, ctx, base);
}

REX_EXTERN(__imp__sub_8222F1B8);
REX_HOOK_RAW(sub_8222F1B8) {  // D3DDevice_SetSamplerState_MagFilter
  Forwarded(kD3DDevice_SetSamplerState_MagFilter, __imp__sub_8222F1B8, ctx, base);
}

REX_EXTERN(__imp__sub_8264D1E0);
REX_HOOK_RAW(sub_8264D1E0) {  // D3DDevice_SetSamplerState_MagFilterZ
  Forwarded(kD3DDevice_SetSamplerState_MagFilterZ, __imp__sub_8264D1E0, ctx, base);
}

REX_EXTERN(__imp__sub_8264D340);
REX_HOOK_RAW(sub_8264D340) {  // D3DDevice_SetSamplerState_MaxAnisotropy
  Forwarded(kD3DDevice_SetSamplerState_MaxAnisotropy, __imp__sub_8264D340, ctx, base);
}

REX_EXTERN(__imp__sub_8222F590);
REX_HOOK_RAW(sub_8222F590) {  // D3DDevice_SetSamplerState_MaxMipLevel
  Forwarded(kD3DDevice_SetSamplerState_MaxMipLevel, __imp__sub_8222F590, ctx, base);
}

REX_EXTERN(__imp__sub_8222F280);
REX_HOOK_RAW(sub_8222F280) {  // D3DDevice_SetSamplerState_MinFilter
  Forwarded(kD3DDevice_SetSamplerState_MinFilter, __imp__sub_8222F280, ctx, base);
}

REX_EXTERN(__imp__sub_8264D100);
REX_HOOK_RAW(sub_8264D100) {  // D3DDevice_SetSamplerState_MinFilterZ
  Forwarded(kD3DDevice_SetSamplerState_MinFilterZ, __imp__sub_8264D100, ctx, base);
}

REX_EXTERN(__imp__sub_8264D510);
REX_HOOK_RAW(sub_8264D510) {  // D3DDevice_SetSamplerState_MinMipLevel
  Forwarded(kD3DDevice_SetSamplerState_MinMipLevel, __imp__sub_8264D510, ctx, base);
}

REX_EXTERN(__imp__sub_8228F2F0);
REX_HOOK_RAW(sub_8228F2F0) {  // D3DDevice_SetSamplerState_MipFilter
  Forwarded(kD3DDevice_SetSamplerState_MipFilter, __imp__sub_8228F2F0, ctx, base);
}

REX_EXTERN(__imp__sub_8264D460);
REX_HOOK_RAW(sub_8264D460) {  // D3DDevice_SetSamplerState_MipMapLodBias
  Forwarded(kD3DDevice_SetSamplerState_MipMapLodBias, __imp__sub_8264D460, ctx, base);
}

REX_EXTERN(__imp__sub_8264D7E0);
REX_HOOK_RAW(sub_8264D7E0) {  // D3DDevice_SetSamplerState_PointBorderEnable
  Forwarded(kD3DDevice_SetSamplerState_PointBorderEnable, __imp__sub_8264D7E0, ctx, base);
}

REX_EXTERN(__imp__sub_8264D298);
REX_HOOK_RAW(sub_8264D298) {  // D3DDevice_SetSamplerState_SeparateZFilterEnable
  Forwarded(kD3DDevice_SetSamplerState_SeparateZFilterEnable, __imp__sub_8264D298, ctx, base);
}

REX_EXTERN(__imp__sub_8264D680);
REX_HOOK_RAW(sub_8264D680) {  // D3DDevice_SetSamplerState_TrilinearThreshold
  Forwarded(kD3DDevice_SetSamplerState_TrilinearThreshold, __imp__sub_8264D680, ctx, base);
}

REX_EXTERN(__imp__sub_8264D730);
REX_HOOK_RAW(sub_8264D730) {  // D3DDevice_SetSamplerState_VGradientExpBias
  Forwarded(kD3DDevice_SetSamplerState_VGradientExpBias, __imp__sub_8264D730, ctx, base);
}

REX_EXTERN(__imp__sub_8264D788);
REX_HOOK_RAW(sub_8264D788) {  // D3DDevice_SetSamplerState_WhiteBorderColorW
  Forwarded(kD3DDevice_SetSamplerState_WhiteBorderColorW, __imp__sub_8264D788, ctx, base);
}

REX_EXTERN(__imp__sub_8265F688);
REX_HOOK_RAW(sub_8265F688) {  // D3DDevice_SetScalerParameters
  Forwarded(kD3DDevice_SetScalerParameters, __imp__sub_8265F688, ctx, base);
}

REX_EXTERN(__imp__sub_82223178);
REX_HOOK_RAW(sub_82223178) {  // D3DDevice_SetScissorRect
  Forwarded(kD3DDevice_SetScissorRect, __imp__sub_82223178, ctx, base);
}

REX_EXTERN(__imp__sub_82658988);
REX_HOOK_RAW(sub_82658988) {  // D3DDevice_SetScreenExtentQueryMode
  Forwarded(kD3DDevice_SetScreenExtentQueryMode, __imp__sub_82658988, ctx, base);
}

REX_EXTERN(__imp__sub_82220B20);
REX_HOOK_RAW(sub_82220B20) {  // D3DDevice_SetShaderGPRAllocation
  Forwarded(kD3DDevice_SetShaderGPRAllocation, __imp__sub_82220B20, ctx, base);
}

REX_EXTERN(__imp__sub_8222A408);
REX_HOOK_RAW(sub_8222A408) {  // D3DDevice_SetStreamSource
  Forwarded(kD3DDevice_SetStreamSource, __imp__sub_8222A408, ctx, base);
}

REX_EXTERN(__imp__sub_8222F410);
REX_HOOK_RAW(sub_8222F410) {  // D3DDevice_SetTexture
  Forwarded(kD3DDevice_SetTexture, __imp__sub_8222F410, ctx, base);
}

REX_EXTERN(__imp__sub_82B96538);
REX_HOOK_RAW(sub_82B96538) {  // D3DDevice_SetTexture_Inline
  Forwarded(kD3DDevice_SetTexture_Inline, __imp__sub_82B96538, ctx, base);
}

REX_EXTERN(__imp__sub_82205700);
REX_HOOK_RAW(sub_82205700) {  // D3DDevice_SetVertexDeclaration
  Forwarded(kD3DDevice_SetVertexDeclaration, __imp__sub_82205700, ctx, base);
}

REX_EXTERN(__imp__sub_8264DB38);
REX_HOOK_RAW(sub_8264DB38) {  // D3DDevice_SetVertexFetchConstant
  Forwarded(kD3DDevice_SetVertexFetchConstant, __imp__sub_8264DB38, ctx, base);
}

REX_EXTERN(__imp__sub_8222A0A8);
REX_HOOK_RAW(sub_8222A0A8) {  // D3DDevice_SetVertexShader
  Forwarded(kD3DDevice_SetVertexShader, __imp__sub_8222A0A8, ctx, base);
}

REX_EXTERN(__imp__sub_82221930);
REX_HOOK_RAW(sub_82221930) {  // D3DDevice_SetVertexShaderConstantB
  Forwarded(kD3DDevice_SetVertexShaderConstantB, __imp__sub_82221930, ctx, base);
}

REX_EXTERN(__imp__sub_8222EFA8);
REX_HOOK_RAW(sub_8222EFA8) {  // D3DDevice_SetVertexShaderConstantFN
  Forwarded(kD3DDevice_SetVertexShaderConstantFN, __imp__sub_8222EFA8, ctx, base);
}

REX_EXTERN(__imp__sub_8264E570);
REX_HOOK_RAW(sub_8264E570) {  // D3DDevice_SetVertexShaderConstantI
  Forwarded(kD3DDevice_SetVertexShaderConstantI, __imp__sub_8264E570, ctx, base);
}

REX_EXTERN(__imp__sub_82222EA0);
REX_HOOK_RAW(sub_82222EA0) {  // D3DDevice_SetViewport
  Forwarded(kD3DDevice_SetViewport, __imp__sub_82222EA0, ctx, base);
}

REX_EXTERN(__imp__sub_8264E1A8);
REX_HOOK_RAW(sub_8264E1A8) {  // D3DDevice_UnsetAll
  Forwarded(kD3DDevice_UnsetAll, __imp__sub_8264E1A8, ctx, base);
}

REX_EXTERN(__imp__sub_82654F90);
REX_HOOK_RAW(sub_82654F90) {  // D3DIndexBuffer_Lock
  Forwarded(kD3DIndexBuffer_Lock, __imp__sub_82654F90, ctx, base);
}

REX_EXTERN(__imp__sub_82655EA8);
REX_HOOK_RAW(sub_82655EA8) {  // D3DLineTexture_LockRect
  Forwarded(kD3DLineTexture_LockRect, __imp__sub_82655EA8, ctx, base);
}

REX_EXTERN(__imp__sub_822088B0);
REX_HOOK_RAW(sub_822088B0) {  // D3DResource_AddRef
  Forwarded(kD3DResource_AddRef, __imp__sub_822088B0, ctx, base);
}

REX_EXTERN(__imp__sub_82654D50);
REX_HOOK_RAW(sub_82654D50) {  // D3DResource_GetDevice
  Forwarded(kD3DResource_GetDevice, __imp__sub_82654D50, ctx, base);
}

REX_EXTERN(__imp__sub_82208110);
REX_HOOK_RAW(sub_82208110) {  // D3DResource_GetType
  Forwarded(kD3DResource_GetType, __imp__sub_82208110, ctx, base);
}

REX_EXTERN(__imp__sub_82208720);
REX_HOOK_RAW(sub_82208720) {  // D3DResource_Release
  Forwarded(kD3DResource_Release, __imp__sub_82208720, ctx, base);
}

REX_EXTERN(__imp__sub_822082A0);
REX_HOOK_RAW(sub_822082A0) {  // D3DSurface_GetDesc
  Forwarded(kD3DSurface_GetDesc, __imp__sub_822082A0, ctx, base);
}

REX_EXTERN(__imp__sub_82656150);
REX_HOOK_RAW(sub_82656150) {  // D3DSurface_LockRect
  Forwarded(kD3DSurface_LockRect, __imp__sub_82656150, ctx, base);
}

REX_EXTERN(__imp__sub_826561E0);
REX_HOOK_RAW(sub_826561E0) {  // D3DSurface_UnlockRect
  Forwarded(kD3DSurface_UnlockRect, __imp__sub_826561E0, ctx, base);
}

REX_EXTERN(__imp__sub_82208928);
REX_HOOK_RAW(sub_82208928) {  // D3DTexture_GetSurfaceLevel
  Forwarded(kD3DTexture_GetSurfaceLevel, __imp__sub_82208928, ctx, base);
}

REX_EXTERN(__imp__sub_82654E80);
REX_HOOK_RAW(sub_82654E80) {  // D3DVertexBuffer_Lock
  Forwarded(kD3DVertexBuffer_Lock, __imp__sub_82654E80, ctx, base);
}

REX_EXTERN(__imp__sub_82654ED0);
REX_HOOK_RAW(sub_82654ED0) {  // D3DVertexBuffer_Unlock
  Forwarded(kD3DVertexBuffer_Unlock, __imp__sub_82654ED0, ctx, base);
}

REX_EXTERN(__imp__sub_82657048);
REX_HOOK_RAW(sub_82657048) {  // D3DVertexShader_Bind
  Forwarded(kD3DVertexShader_Bind, __imp__sub_82657048, ctx, base);
}

REX_EXTERN(__imp__sub_82655DD8);
REX_HOOK_RAW(sub_82655DD8) {  // D3DVolumeTexture_GetVolumeLevel
  Forwarded(kD3DVolumeTexture_GetVolumeLevel, __imp__sub_82655DD8, ctx, base);
}

REX_EXTERN(__imp__sub_82656178);
REX_HOOK_RAW(sub_82656178) {  // D3DVolume_GetDesc
  Forwarded(kD3DVolume_GetDesc, __imp__sub_82656178, ctx, base);
}

REX_EXTERN(__imp__sub_826561C0);
REX_HOOK_RAW(sub_826561C0) {  // D3DVolume_LockBox
  Forwarded(kD3DVolume_LockBox, __imp__sub_826561C0, ctx, base);
}

REX_EXTERN(__imp__sub_82661C38);
REX_HOOK_RAW(sub_82661C38) {  // D3D_AllocateEdramMemory
  Forwarded(kD3D_AllocateEdramMemory, __imp__sub_82661C38, ctx, base);
}

REX_EXTERN(__imp__sub_826571D8);
REX_HOOK_RAW(sub_826571D8) {  // D3D_CDevice_AddCommandsToPrimaryBuffer
  Forwarded(kD3D_CDevice_AddCommandsToPrimaryBuffer, __imp__sub_826571D8, ctx, base);
}

REX_EXTERN(__imp__sub_8222D240);
REX_HOOK_RAW(sub_8222D240) {  // D3D_CDevice_BeginRingAlloc
  Forwarded(kD3D_CDevice_BeginRingAlloc, __imp__sub_8222D240, ctx, base);
}

REX_EXTERN(__imp__sub_8228F558);
REX_HOOK_RAW(sub_8228F558) {  // D3D_CDevice_BlockOnFence
  Forwarded(kD3D_CDevice_BlockOnFence, __imp__sub_8228F558, ctx, base);
}

REX_EXTERN(__imp__sub_82210340);
REX_HOOK_RAW(sub_82210340) {  // D3D_CDevice_BlockOnPrimaryRange
  Forwarded(kD3D_CDevice_BlockOnPrimaryRange, __imp__sub_82210340, ctx, base);
}

REX_EXTERN(__imp__sub_8222D610);
REX_HOOK_RAW(sub_8222D610) {  // D3D_CDevice_BlockOnSecondaryPosition
  Forwarded(kD3D_CDevice_BlockOnSecondaryPosition, __imp__sub_8222D610, ctx, base);
}

REX_EXTERN(__imp__sub_822108B8);
REX_HOOK_RAW(sub_822108B8) {  // D3D_CDevice_CreateInvalidateBuffer
  Forwarded(kD3D_CDevice_CreateInvalidateBuffer, __imp__sub_822108B8, ctx, base);
}

REX_EXTERN(__imp__sub_82657160);
REX_HOOK_RAW(sub_82657160) {  // D3D_CDevice_MarkAsOutOfMemory
  Forwarded(kD3D_CDevice_MarkAsOutOfMemory, __imp__sub_82657160, ctx, base);
}

REX_EXTERN(__imp__sub_826572C8);
REX_HOOK_RAW(sub_826572C8) {  // D3D_CDevice_QueueIndirectBuffers
  Forwarded(kD3D_CDevice_QueueIndirectBuffers, __imp__sub_826572C8, ctx, base);
}

REX_EXTERN(__imp__sub_822873E8);
REX_HOOK_RAW(sub_822873E8) {  // D3D_CDevice_StartNewSegment
  Forwarded(kD3D_CDevice_StartNewSegment, __imp__sub_822873E8, ctx, base);
}

REX_EXTERN(__imp__sub_822084F0);
REX_HOOK_RAW(sub_822084F0) {  // D3D_DestroyResource
  Forwarded(kD3D_DestroyResource, __imp__sub_822084F0, ctx, base);
}

REX_EXTERN(__imp__Direct3D_CreateDevice);
REX_HOOK_RAW(Direct3D_CreateDevice) {  // Direct3D_CreateDevice
  Forwarded(kDirect3D_CreateDevice, __imp__Direct3D_CreateDevice, ctx, base);
}

REX_EXTERN(__imp__Direct3DDevice_Release);
REX_HOOK_RAW(Direct3DDevice_Release) {  // D3DDevice_Release
  Forwarded(kD3DDevice_Release, __imp__Direct3DDevice_Release, ctx, base);
}

REX_EXTERN(__imp__Direct3DDevice_CreateTexture);
REX_HOOK_RAW(Direct3DDevice_CreateTexture) {  // D3DDevice_CreateTexture
  Forwarded(kD3DDevice_CreateTexture, __imp__Direct3DDevice_CreateTexture, ctx, base);
}

REX_EXTERN(__imp__D3DDevice_BlockUntilIdle);
REX_HOOK_RAW(D3DDevice_BlockUntilIdle) {  // D3DDevice_BlockUntilIdle
  Forwarded(kD3DDevice_BlockUntilIdle, __imp__D3DDevice_BlockUntilIdle, ctx, base);
}

REX_EXTERN(__imp__D3D__UninitializeDevice);
REX_HOOK_RAW(D3D__UninitializeDevice) {  // D3D_UninitializeDevice
  Forwarded(kD3D_UninitializeDevice, __imp__D3D__UninitializeDevice, ctx, base);
}
