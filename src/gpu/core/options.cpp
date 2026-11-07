// The renut_plume_* cvars of the Plume GPU. Declared in internal.h.
#include "gpu/core/internal.h"

REXCVAR_DEFINE_INT32(renut_plume_pipeline_lookup_ms, 4, "reNut/Plume",
                     "Milliseconds per frame the command thread may spend asking the driver's cache for "
                     "new pipelines it already has, so their draws appear at once; past it they go to the "
                     "workers. 0 leaves every new pipeline to the workers.")
    .range(0, 33);
REXCVAR_DEFINE_INT32(renut_plume_hiz, 2, "reNut/Plume",
                     "Hi-Z culling: 0 off, 1 only counts what could be dropped, 2 drops the draws whose bounding box "
                     "lies wholly outside the view frustum, the scissor or an enabled user clip plane (docs/performance/HIZ.md)");
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

