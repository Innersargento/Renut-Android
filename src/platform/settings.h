// The app's own options, one setting each under the scope "renut" (renut.<name>). The player's file
// (files/renut.toml) and the sidebar reach them by name through the SDK's settings registry.
#pragma once
#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace renut {

class Settings final : public rex::SettingOwner {
 public:
  Settings() : SettingOwner("renut") {}

  REX_SETTING(rex::i32, d3d_report_swaps, 0,
              "Log the most called Direct3D functions every N swaps (0 = off). Off by "
               "default: the game polls D3D_CBlocker_Check tens of millions of times a "
               "second while it waits for a fence, and counting every call cost a quarter "
               "of the process's cycles in one atomic increment");
  REX_SETTING(bool, d3d_stats, false,
              "Keep the per-call and per-draw counters of the native Direct3D report "
               "(files/native-d3d.txt). Off by default: they are atomic increments on arrays "
               "shared between threads, on every call and draw");
  REX_SETTING(rex::i32, plume_pipeline_lookup_ms, 4,
              "Milliseconds per frame the command thread may spend asking the driver's cache for "
               "new pipelines it already has, so their draws appear at once; past it they go to the "
               "workers. 0 leaves every new pipeline to the workers.", rex::Range{0, 33});
  REX_SETTING(rex::i32, plume_pipeline_wait_ms, 8,
              "Milliseconds per frame the command thread may wait for the pipelines of draws that have neither "
               "theirs nor a ready sibling's, instead of leaving them undrawn for as long as the compile takes "
               "(a flicker on drivers that compile slowly, such as Samsung's Xclipse). 0 never waits. Read at "
               "start.", rex::Range{0, 33});
  REX_SETTING(rex::i32, plume_hiz, 2,
              "Hi-Z culling: 0 off, 1 only counts what could be dropped, 2 drops the draws whose bounding box "
               "lies wholly outside the view frustum, the scissor or an enabled user clip plane (docs/performance/HIZ.md)");
  REX_SETTING(rex::i32, plume_adaptive_patches, 1,
              "Tessellate adaptive triangle patches (the water) at the levels the game works out for "
               "each edge, a patch off screen being one triangle, instead of every patch at the draw's "
               "highest level: 0 off, 1 on", rex::Range{0, 1});
  REX_SETTING(rex::i32, plume_poly_offset_percent, 1600,
              "The game's polygon offset (PA_SU_POLY_OFFSET_*), as a percent of what it asks (0 = off). "
               "Ground decals - the tire marks and skid marks of a vehicle, footprints - lie on the "
               "terrain and ask to be pulled toward the camera; without it they fight the surface under "
               "them for depth and flicker (the pier). 100 is the offset as the game gives it, which left the pier's "
               "marks flickering; 1600 stopped it (tested on the device, 24/10). Followed live.", rex::Range{0, 1600});
  REX_SETTING(bool, plume_decal_log, false,
              "Debug: log each distinct state of the draws that ask for a polygon offset (shaders, depth "
               "function, the offset asked and the bias given to Vulkan) and a count every 300 frames. "
               "Followed live.");
  REX_SETTING(bool, plume_no_shadows, false,
              "Drop the shadow casters (depth-only draws into the shadow maps): the scene is drawn "
               "without shadows.");
  REX_SETTING(bool, plume_draw_callers, false,
              "Debug: tag each draw with the host stack of the game code that made it, and log every 600 "
               "frames the callers with the most draws and Hi-Z culls (tools/draw_callers.py names them). "
               "Followed live; slows the game a little while on.");
  REX_SETTING(bool, plume_ui_depth_fix, true,
              "A draw that tests depth LESS or LESS_EQUAL under a reversed viewport (z scale -1, offset 1) is drawn "
               "without the depth test: the scene's depth is cleared to 0 and tested GREATER, so such a test never "
               "passes (the loading screen's spinning gear). Followed live.");
  REX_SETTING(rex::i32, plume_water_tess_percent, 100,
              "Percent of the game's tessellation level the water grid uses (100 = as the game sets it).", rex::Range{10, 100});
  REX_SETTING(rex::i32, plume_shadow_min_indices, 0,
              "Shadow casters (depth-only draws into a shadow map) with fewer indices than this are "
               "dropped; 0 keeps them all.", rex::Range{0, 1000000});
  REX_SETTING(rex::i32, plume_low_memory, -1,
              "Keep less on the GPU for a device with little RAM: -1 decides from the device (under 6 GB), "
               "0 off, 1 on.");
  REX_SETTING(rex::i32, plume_texture_max_dim, 0,
              "Largest side a guest texture keeps on the host; the top mips of a larger one are dropped. "
               "0 follows low-memory mode (512 there, no limit otherwise).");
  REX_SETTING(rex::i32, plume_mali_rebase_indices, 1,
              "ARM's Mali driver gives a vertex shader the wrong vertex index in an indexed draw whose "
               "smallest index is not zero, and the meshes a shader fetches by that index come out "
               "stretched or black. Such a draw's indices are sent less their smallest, which becomes the "
               "draw's vertex offset: 0 off, 1 for the shaders that fetch by themselves, 2 for every "
               "indexed draw. No effect on other drivers.");
  REX_SETTING(rex::i32, plume_bc_textures, 1,
              "Upload the guest's DXT1/2/3/4/5, DXT5A and DXN 2D textures as the BC1-BC5 blocks they "
               "already are (a quarter to an eighth of the RGBA8 the decoder makes, no CPU decode) on a GPU "
               "that samples BC (Adreno): 0 off, 1 on.", rex::Range{0, 1});
  REX_SETTING(rex::i32, plume_texture_quality, 0,
              "Texture quality: 0 full, 1 half, 2 quarter. A 2D texture the game samples with mip "
               "filtering (the world; the interface and fonts are read from the base level alone and "
               "stay sharp) drops its top one or two mips, never below 64 px. Live:", rex::Range{0, 2});
  REX_SETTING(rex::i32, plume_aniso_max, 4,
              "The most anisotropic filtering a texture fetch gets: 1 (none), 2, 4, 8 or 16 samples. The "
               "game asks for up to 16 on some surfaces, which a phone's GPU pays for on every such texel; "
               "16 gives every fetch what it asks. Live: a sampler made for another maximum is made anew when the "
               "game next draws with it.", rex::Range{1, 16});
  REX_SETTING(bool, plume_no_bloom, false,
              "Make the bloom's bright pass write black, so the blur chain and the final blend add no glow. "
               "Live: the pass's pipeline is made again when the option changes.");
  REX_SETTING(rex::i32, plume_shadow_quality, 0,
              "Shadow map resolution: 0 full, 1 three quarters, 2 half (a side of the dynamic maps; "
               "the cached atlas is not touched). The maps are drawn and resolved smaller and sampled "
               "with the same coordinates. Read at start.", rex::Range{0, 2});
  REX_SETTING(rex::i32, plume_internal_resolution, 0,
              "Internal resolution of the scene: 0 full, 1 three quarters (960x540), 2 half (640x360) of "
               "the guest's 1280x720. The scene's colour and depth surfaces are drawn smaller, resolved "
               "smaller and shown stretched. Read at start.", rex::Range{0, 2});
  REX_SETTING(std::string, aspect_ratio, "16:9",
              "Aspect ratio the game renders at (guest/widescreen.cpp): the height stays 720 and the width grows, "
               "so a wider screen sees more of the world at the sides and the interface stays centred at 16:9. "
               "\"device\" takes the shape of the game's surface. Narrower than 16:9 stays 16:9. Read at start.",
              rex::Allowed("16:9", "18:9", "19.5:9", "20:9", "21:9", "device"));
  REX_SETTING(rex::i32, field_of_view, 100,
              "Field of view, in percent of the game's own (guest/field_of_view.cpp): 100 is the game's, up to 150 "
               "shows more of the world. Cutscenes widen too. Followed live.", rex::Range{100, 150});
  REX_SETTING(bool, plume_precompile, true,
              "Record every pipeline the game uses (files/plume-recipes.bin) and build them all at the "
               "next start; a scene load waits until they are built. Not in low-memory mode, which only "
               "records.");
  REX_SETTING(bool, cheats, false,
              "Master switch of the cheats below; none applies while it is off");
  REX_SETTING(bool, cheat_infinite_health, false,
              "Health never decreases");
  REX_SETTING(bool, cheat_infinite_fuel_ammo, false,
              "Vehicle fuel and ammo are not consumed");
  REX_SETTING(bool, cheat_infinite_parts, false,
              "Using a vehicle part never lowers its count");
  REX_SETTING(bool, cheat_no_timer, false,
              "Mission timers never run out");
  REX_SETTING(bool, cheat_no_notes_spent, false,
              "Spending notes costs none");
  REX_SETTING(bool, cheat_vehicles_anywhere, false,
              "Change and build vehicles from the pause menu outside Showdown Town");
  REX_SETTING(bool, cheat_extended_build_range, false,
              "A larger area to build in at Mumbo's Motors");
  REX_SETTING(bool, cheat_super_punch, false,
              "Hits on anything but Banjo do enormous damage");
  REX_SETTING(std::string, cheat_banjo_skin, "default",
              "Banjo's outfit", rex::Allowed("default", "robot", "tuxedo"));
  REX_SETTING(bool, physics_whole_steps, true,
              "Advance physics by whole 1/60 s steps each frame, carrying the remainder, as the console's "
               "30 fps did. Off: the game's own stepping, which flings Banjo about on ledges, tightropes "
               "and ladders at 60 fps. At 30 fps both are the same.");
  REX_SETTING(rex::i32, msaa, 0,
              "Multisampling of the scene, drawn in one band (no predicated tiling): 0 off, 1 2x, 2 4x. "
               "The game's MSAA surfaces are kept at the screen's size and the host resolves their samples "
               "(colour averaged, depth from the first sample). Followed while the game runs", rex::Range{0, 2});
  REX_SETTING(rex::i32, fps_target, 30,
              "Frame rate the game is held to: 30 is its own, 31-240 presents "
               "immediately and paces to that rate, 0 is unlimited. Changes live from the sidebar "
               "(plume/frame_pacing.h)", rex::Range{0, 240});
  REX_SETTING(bool, pump_loading_hold, true,
              "While a scene start is held for its pipelines, call the game's own stall pump so the loading "
               "screen keeps animating (the Pong's ball) instead of standing still.");
  REX_SETTING(bool, pump_loading_waits, true,
              "While a scene loads, the main thread's waits of the game's job system (sub_821E04F0) are cut into "
               "16 ms slices and the game's stall pump runs between them, so the loading screen draws frames "
               "during the world build as it does on a disc.");
  REX_SETTING(bool, audio_threads_boost, true,
              "Run the threads that make the sound (the game's mixer, the SDK's audio worker and XMA decoder) at "
               "nice -10 on the fast cores, so the mixed frames keep ahead of the device next to the pipeline "
               "compiles and the GPU backend (platform/audio_threads.h). Read at start.");
  REX_SETTING(bool, adpf, true,
              "Report each frame's work to Android's performance hint session (ADPF) so the system "
               "holds the CPU at the lowest clocks that keep the game's pace");
  REX_SETTING(bool, log_to_file, false,
              "Write the runtime's log to a file of Documents/renut at each start");
  REX_SETTING(bool, logcat_capture, false,
              "Write the app's logcat to a file of Documents/renut while the game runs");
  REX_SETTING(bool, plume_vsync, true,
              "Present through the display's vertical sync (FIFO). Off asks for immediate "
               "presentation where the driver has it, so frames are not held to the refresh rate");
  REX_SETTING(bool, plume_present_pass, true,
              "Show the frame with one render pass on the swapchain image (cleared as the pass begins, the "
               "frame drawn as a textured rectangle) instead of a clear and a blit of the whole image. Read "
               "at start.");
  REX_SETTING(bool, plume_present_fit, true,
              "Make the swapchain at the size of the game's frame (the screen's shape, the frame's lines) "
               "and leave the scaling to the display's compositor, instead of filling a swapchain of the "
               "screen's own resolution every frame. Read at start.");
  REX_SETTING(bool, plume_present_prerotate, true,
              "Draw the frame already turned the way the screen is mounted (the swapchain's preTransform is "
               "the surface's current one), so the compositor does not turn every frame itself, which on "
               "many phones it does with the GPU. Needs renut_plume_present_pass. Read at start.");
};

// The options of this process.
Settings& GetSettings();

}  // namespace renut
