// SPIR-V of the renderer's own shaders (src/plume/shaders/*), compiled to arrays by
// tools/shaders/build_host_shaders.sh. Sizes are part of the declarations so sizeof gives bytes.
#pragma once
#include <cstdint>

extern const uint32_t k_depth_resolve_comp[436];  // depth_resolve.comp
extern const uint32_t k_depth_resolve_ms_comp[593];  // depth_resolve_ms.comp
extern const uint32_t k_depth_stencil_pack_comp[545];  // depth_stencil_pack.comp
extern const uint32_t k_patch_expand_comp[2492];  // patch_expand.comp
extern const uint32_t k_patch_levels_comp[1774];  // patch_levels.comp
extern const uint32_t k_point_list_geom[1624];  // point_list.geom
extern const uint32_t k_point_list_position_geom[1558];  // point_list_position.geom
extern const uint32_t k_present_frag[216];  // present.frag
extern const uint32_t k_present_vert[350];  // present.vert
extern const uint32_t k_rect_list_geom[3171];  // rect_list.geom
extern const uint32_t k_rect_list_position_geom[3096];  // rect_list_position.geom
