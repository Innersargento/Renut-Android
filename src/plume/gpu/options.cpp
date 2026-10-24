// The renut_plume_* cvars of the Plume GPU. Declared in internal.h.
#include "internal.h"

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
                     "lies wholly outside the view frustum, the scissor or an enabled user clip plane (docs/performance/HIZ.md)");
REXCVAR_DEFINE_INT32(renut_plume_skip, 0, "reNut/Plume",
                     "Debug: the plume-skip bits (see the skip_mask_ tests in src/plume/gpu), e.g. 1024 no depth test, "
                     "131072 no culling, 4096 no alpha to mask. Other bits also turn off Hi-Z and the draw windows. "
                     "Followed live.");
REXCVAR_DEFINE_BOOL(renut_plume_zero_census, false, "reNut/Plume",
                    "Debug: run every draw inside an occlusion query and sum the draws that leave no sample by their "
                    "state, into <files>/plume-zero-census.txt every 300 frames and when turned off. Followed live.");
REXCVAR_DEFINE_BOOL(renut_plume_stack_wrap, true, "reNut/Plume",
                    "Address a stacked texture's layers as its fetch constant says (repeat, mirror, clamp) instead of "
                    "always clamping them to the ends of the stack. The water walks a 12-layer stack in time with a "
                    "coordinate far past the stack's end and relies on the layer index repeating; clamped, it sat on "
                    "the last layer and the waves stood still. Followed live.");
REXCVAR_DEFINE_BOOL(renut_plume_shader_cache, true, "reNut/Plume",
                    "Remember across frames which shader lives at a guest address and length, trusting it "
                    "until the guest writes there (write-watched), instead of hashing the microcode of "
                    "every shader a frame uses again");
REXCVAR_DEFINE_BOOL(renut_plume_key_reuse, true, "reNut/Plume",
                    "Key a draw's pipeline by one hash of its vertex shader's fetch table, kept on the "
                    "shader, instead of laying every specialization constant of the table into the key "
                    "each draw");
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
REXCVAR_DEFINE_INT32(renut_plume_poly_offset_percent, 1600, "reNut/Plume",
                     "The game's polygon offset (PA_SU_POLY_OFFSET_*), as a percent of what it asks (0 = off). "
                     "Ground decals - the tire marks and skid marks of a vehicle, footprints - lie on the "
                     "terrain and ask to be pulled toward the camera; without it they fight the surface under "
                     "them for depth and flicker (the pier). 100 is the offset as the game gives it, which left the pier's "
                     "marks flickering; 1600 stopped it (tested on the device, 24/10). Followed live.")
    .range(0, 1600);
REXCVAR_DEFINE_BOOL(renut_plume_decal_log, false, "reNut/Plume",
                    "Debug: log each distinct state of the draws that ask for a polygon offset (shaders, depth "
                    "function, the offset asked and the bias given to Vulkan) and a count every 300 frames. "
                    "Followed live.");
REXCVAR_DEFINE_BOOL(renut_plume_no_shadows, false, "reNut/Plume",
                    "Drop the shadow casters (depth-only draws into the shadow maps): the scene is drawn "
                    "without shadows.");
REXCVAR_DEFINE_INT32(renut_plume_water_tess_percent, 100, "reNut/Plume",
                     "Percent of the game's tessellation level the water grid uses (100 = as the game sets it).")
    .range(10, 100);
REXCVAR_DEFINE_INT32(renut_plume_shadow_min_indices, 0, "reNut/Plume",
                     "Shadow casters (depth-only draws into a shadow map) with fewer indices than this are "
                     "dropped; 0 keeps them all.")
    .range(0, 1000000);
REXCVAR_DEFINE_INT32(renut_plume_fence_sleep_us, 250, "reNut/Plume",
                     "Microseconds the fence wait sleeps once the GPU is clearly not moving; 0 is the busy wait.")
    .range(0, 20000);
REXCVAR_DEFINE_INT32(renut_plume_low_memory, -1, "reNut/Plume",
                     "Keep less on the GPU for a device with little RAM: -1 decides from the device (under 6 GB), "
                     "0 off, 1 on.");
REXCVAR_DEFINE_INT32(renut_plume_texture_max_dim, 0, "reNut/Plume",
                     "Largest side a guest texture keeps on the host; the top mips of a larger one are dropped. "
                     "0 follows low-memory mode (512 there, no limit otherwise).");
REXCVAR_DEFINE_INT32(renut_plume_bc_textures, 1, "reNut/Plume",
                     "Upload the guest's DXT1/2/3/4/5, DXT5A and DXN 2D textures as the BC1-BC5 blocks they "
                     "already are (a quarter to an eighth of the RGBA8 the decoder makes, no CPU decode) on a GPU "
                     "that samples BC (Adreno): 0 off, 1 on.")
    .range(0, 1);
REXCVAR_DEFINE_INT32(renut_plume_texture_quality, 0, "reNut/Plume",
                     "Texture quality: 0 full, 1 half, 2 quarter. A 2D texture the game samples with mip "
                     "filtering (the world; the interface and fonts are read from the base level alone and "
                     "stay sharp) drops its top one or two mips, never below 64 px. Live:")
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
