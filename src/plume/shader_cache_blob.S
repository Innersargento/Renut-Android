// Embeds the recompiled shaders' smol-v blob (generated/shaders) in the
// library. The path comes from RENUT_SHADER_BLOB, set by CMakeLists.txt.
#if defined(_WIN32)
    .section .rdata,"dr"
#else
    .section .rodata
#endif
    .balign 16
    .global g_spirvCache
    .global g_spirvCacheEnd
g_spirvCache:
    .incbin RENUT_SHADER_BLOB
g_spirvCacheEnd:
