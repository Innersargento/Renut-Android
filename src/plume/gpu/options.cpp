// The renut_plume_* cvars of the Plume GPU. Declared in internal.h.
#include "internal.h"

REXCVAR_DEFINE_BOOL(renut_plume_async_pipelines, true, "reNut/Plume",
                    "Compile new Plume graphics pipelines on workers; until one is ready its draw is drawn "
                    "with a sibling pipeline or skipped. Pipelines the driver already has are taken at the "
                    "draw. Draws that export to memory always wait for theirs.");
REXCVAR_DEFINE_INT32(renut_plume_pipeline_lookup_ms, 4, "reNut/Plume",
                     "Milliseconds per frame the command thread may spend asking the driver's cache for "
                     "new pipelines it already has, so their draws appear at once; past it they go to the "
                     "workers. 0 leaves every new pipeline to the workers.")
    .range(0, 33);
REXCVAR_DEFINE_INT32(renut_plume_bool_spec, 0, "reNut/Plume",
                     "How shaders see the Xenos boolean registers: 0 read from the shared block every draw "
                     "(about a quarter of the pipelines to compile), 1 as specialization constants (a "
                     "pipeline per combination of the bits a pair of shaders tests)")
    .range(0, 1);
REXCVAR_DEFINE_INT32(renut_plume_defer_draws, 2, "reNut/Plume",
                     "Record draws in windows grouped by pipeline: 0 off, 1 depth-only draws (the shadow "
                     "maps), 2 also opaque colour draws that test and write depth (reordering them is "
                     "exact except for depth ties; measured 11/10: -37% pipeline binds, -3% GPU cycles, "
                     "image unchanged)")
    .range(0, 2);
REXCVAR_DEFINE_INT32(renut_plume_hiz, 2, "reNut/Plume",
                     "Hi-Z culling: 0 off, 1 only counts what could be dropped, 2 drops the draws whose bounding box "
                     "lies wholly outside the view frustum or the scissor (exact; files/plume-hiz overrides)");
REXCVAR_DEFINE_BOOL(renut_plume_shader_cache, true, "reNut/Plume",
                    "Remember across frames which shader lives at a guest address and length, trusting it "
                    "until the guest writes there (write-watched), instead of hashing the microcode of "
                    "every shader a frame uses again (files/plume-shadercache overrides)");
REXCVAR_DEFINE_BOOL(renut_plume_key_reuse, true, "reNut/Plume",
                    "Key a draw's pipeline by one hash of its vertex shader's fetch table, kept on the "
                    "shader, instead of laying every specialization constant of the table into the key "
                    "each draw (files/plume-keyreuse overrides)");
REXCVAR_DEFINE_INT32(renut_plume_fetch_spec, 1, "reNut/Plume",
                     "Give each vertex shader's live vertex fetch table to the driver as specialization "
                     "constants, so that the format switches of its fetch sites fold away: 0 off, 1 on")
    .range(0, 1);
REXCVAR_DEFINE_INT32(renut_plume_depth_strip, 1, "reNut/Plume",
                     "Draw depth-only draws (no fragment shader: the shadow maps) with a vertex shader "
                     "that has lost its interpolant outputs, which nothing reads: 0 off, 1 on")
    .range(0, 1);
REXCVAR_DEFINE_INT32(renut_plume_rel_constants, 2, "reNut/Plume",
                     "How a vertex shader's relative constant reads (a0, aL: skinning matrices) are served: "
                     "0 by the file's four blocks, 1 by the whole file as one block, 2 by that block "
                     "declared too large for the driver to promote into the constant file")
    .range(0, 2);
REXCVAR_DEFINE_INT32(renut_plume_adaptive_patches, 1, "reNut/Plume",
                     "Tessellate adaptive triangle patches (the water) at the levels the game works out for "
                     "each edge, a patch off screen being one triangle, instead of every patch at the draw's "
                     "highest level: 0 off, 1 on")
    .range(0, 1);
REXCVAR_DEFINE_BOOL(renut_plume_no_shadows, false, "reNut/Plume",
                    "Drop the shadow casters (depth-only draws into the shadow maps): the scene is drawn "
                    "without shadows.");
REXCVAR_DEFINE_BOOL(renut_plume_no_bloom, false, "reNut/Plume",
                    "Make the bloom's bright pass write black, so the blur chain and the final blend add "
                    "no glow.");
REXCVAR_DEFINE_BOOL(renut_plume_memexport_readback, false, "reNut/Plume",
                    "Copy shader exports back to guest memory (experimental; may overwrite reused memory).");
REXCVAR_DEFINE_BOOL(renut_plume_memexport_fast, true, "reNut/Plume",
                    "Batch Plume memexport readback across frames; off waits for the current frame.");
REXCVAR_DEFINE_INT32(renut_plume_low_memory, -1, "reNut/Plume",
                     "Keep less on the GPU for a device with little RAM: -1 decides from the device (under 6 GB), "
                     "0 off, 1 on.");
REXCVAR_DEFINE_INT32(renut_plume_texture_max_dim, 0, "reNut/Plume",
                     "Largest side a guest texture keeps on the host; the top mips of a larger one are dropped. "
                     "0 follows low-memory mode (512 there, no limit otherwise).");
REXCVAR_DEFINE_INT32(renut_plume_bc_textures, 1, "reNut/Plume",
                     "Upload the guest's DXT1/2/3/4/5, DXT5A and DXN 2D textures as the BC1-BC5 blocks they "
                     "already are (a quarter to an eighth of the RGBA8 the decoder makes, no CPU decode) on a GPU "
                     "that samples BC (Adreno): 0 off, 1 on. Live: files/plume-bc.")
    .range(0, 1);
REXCVAR_DEFINE_INT32(renut_plume_texture_quality, 0, "reNut/Plume",
                     "Texture quality: 0 full, 1 half, 2 quarter. A 2D texture the game samples with mip "
                     "filtering (the world; the interface and fonts are read from the base level alone and "
                     "stay sharp) drops its top one or two mips, never below 64 px. Live: "
                     "files/plume-texquality.")
    .range(0, 2);
REXCVAR_DEFINE_INT32(renut_plume_shadow_quality, 0, "reNut/Plume",
                     "Shadow map resolution: 0 full, 1 three quarters, 2 half (a side of the dynamic maps; "
                     "the cached atlas is not touched). The maps are drawn and resolved smaller and sampled "
                     "with the same coordinates. Read at start.")
    .range(0, 2);
REXCVAR_DEFINE_INT32(renut_plume_internal_resolution, 0, "reNut/Plume",
                     "Internal resolution of the scene: 0 full, 1 three quarters (960x540), 2 half (640x360) of "
                     "the guest's 1280x720. The scene's colour and depth surfaces are drawn smaller, resolved "
                     "smaller and shown stretched. Read at start.")
    .range(0, 2);
REXCVAR_DEFINE_BOOL(renut_plume_precompile, true, "reNut/Plume",
                    "Record every pipeline the game uses (files/plume-recipes.bin) and build them all at the "
                    "next start; a scene load waits until they are built. Not in low-memory mode, which only "
                    "records.");

// The SDK's `vsync` cvar (rex/graphics/flags.h): the Xenos GPU plugin defined it, and this app
// has no plugin. Off ticks the guest's vertical blank every millisecond.
REXCVAR_DEFINE_BOOL(vsync, true, "GPU", "Enable vertical sync");

// Read by the shader analysis (xenos/translator.cpp), which the SDK's GPU plugin used to configure.
REXCVAR_DEFINE_STRING(dump_shaders, "", "GPU", "Path to dump shaders to");
