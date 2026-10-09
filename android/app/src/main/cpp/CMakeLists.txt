cmake_minimum_required(VERSION 3.22)
project(renut_android LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(RENUT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(PLUME_ROOT "${RENUT_ROOT}/external/plume")
if(NOT EXISTS "${PLUME_ROOT}/plume_vulkan.cpp")
    message(FATAL_ERROR "Execute tools/bootstrap-android.ps1 antes de compilar.")
endif()

# Generate the loader seam without changing the pinned upstream checkout.
# The copy is written at configure time, so an edit to the source has to
# reconfigure: otherwise the build keeps compiling the stale copy.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PLUME_ROOT}/plume_vulkan.cpp")
file(READ "${PLUME_ROOT}/plume_vulkan.cpp" PLUME_SOURCE)
string(FIND "${PLUME_SOURCE}" "VkResult res = volkInitialize();" LOADER_POSITION)
if(LOADER_POSITION EQUAL -1)
    message(FATAL_ERROR "A inicializacao do Plume mudou; revisar a adaptacao do driver.")
endif()
string(REPLACE "VkResult res = volkInitialize();"
    "VkResult res = renutInitializeVulkanLoader();" PLUME_SOURCE "${PLUME_SOURCE}")
string(REPLACE "#include \"plume_vulkan.h\""
    "#include \"plume_vulkan.h\"\nextern VkResult renutInitializeVulkanLoader();\nextern VkResult renutCreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*);\nextern VkResult renutCreateComputePipelines(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*);"
    PLUME_SOURCE "${PLUME_SOURCE}")
# Upstream compiles every pipeline with no cache, so the driver recompiles all of
# them on every launch. Entering Showdown Town spent 67% of its frame time inside
# vkCreateGraphicsPipelines. Every pipeline goes through reNut instead, which
# looks it up in the cache kept on disk and compiles only what is not there
# (plume_renderer.cpp).
foreach(PLUME_PIPELINE_CALL GraphicsPipelines ComputePipelines)
    string(FIND "${PLUME_SOURCE}" "vkCreate${PLUME_PIPELINE_CALL}(device->vk, VK_NULL_HANDLE," PLUME_PIPELINE_AT)
    if(PLUME_PIPELINE_AT EQUAL -1)
        message(FATAL_ERROR "A criacao de pipelines do Plume mudou; revisar o cache de pipelines.")
    endif()
    string(REPLACE "vkCreate${PLUME_PIPELINE_CALL}(device->vk, VK_NULL_HANDLE,"
        "renutCreate${PLUME_PIPELINE_CALL}(device->vk, VK_NULL_HANDLE,"
        PLUME_SOURCE "${PLUME_SOURCE}")
endforeach()

# VdSwap's native path uploads through TRANSFER before touching color output.
# Waiting only at COLOR_ATTACHMENT_OUTPUT does not protect those writes.
string(REPLACE "const VkPipelineStageFlags waitStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;"
    "const VkPipelineStageFlags waitStages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;"
    PLUME_SOURCE "${PLUME_SOURCE}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/plume_vulkan_renut.cpp" "${PLUME_SOURCE}")
add_library(renut_plume STATIC "${CMAKE_CURRENT_BINARY_DIR}/plume_vulkan_renut.cpp")
set_target_properties(renut_plume PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(renut_plume PUBLIC "${PLUME_ROOT}"
    "${PLUME_ROOT}/contrib/volk" "${PLUME_ROOT}/contrib/Vulkan-Headers/include"
    "${PLUME_ROOT}/contrib/VulkanMemoryAllocator/include")
target_compile_definitions(renut_plume PRIVATE NDEBUG)
add_subdirectory("${RENUT_ROOT}/external/libadrenotools" adrenotools)
add_library(renut_probe SHARED probe.cpp)
target_link_libraries(renut_probe PRIVATE renut_plume adrenotools android log dl)
target_link_options(renut_probe PRIVATE "-Wl,-z,max-page-size=16384")

add_subdirectory("${RENUT_ROOT}/android/rexglue" rexglue)
