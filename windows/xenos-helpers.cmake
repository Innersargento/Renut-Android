# The decoding helpers reNut takes from the SDK's Xenos GPU sources, without the plugin.
#
# renut_xenos_helpers(<target> <sdk root> <sdk target to copy settings from, or empty> <runtime target>)
#
# Shared by the Windows and Android builds (android/native/CMakeLists.txt includes this file).
# The sources are the SDK's, unmodified; the list is what the renderer links against:
#   register_file.cpp, registers.cpp   RegisterFile and the register layouts
#   format/ucode.cpp             the ucode opcode tables
#   pipeline/shader/shader.cpp, translator.cpp, translator_disasm.cpp
#                                Shader and its microcode analysis
#   pipeline/shader/interpreter.cpp   the ucode interpreter (Hi-Z's vertex position model)
#   util/draw.cpp                scissor, resolve and depth-control decoding
#   flags.cpp                    the cvars those read
function(renut_xenos_helpers target sdk_root original runtime)
    set(dir "${sdk_root}/src/graphics")
    add_library(${target} STATIC
        "${dir}/flags.cpp" "${dir}/register_file.cpp" "${dir}/registers.cpp" "${dir}/format/ucode.cpp"
        "${dir}/pipeline/shader/shader.cpp" "${dir}/pipeline/shader/translator.cpp"
        "${dir}/pipeline/shader/translator_disasm.cpp" "${dir}/pipeline/shader/interpreter.cpp"
        "${dir}/util/draw.cpp")
    if(original AND TARGET ${original})
        foreach(property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS
                INTERFACE_INCLUDE_DIRECTORIES INTERFACE_COMPILE_DEFINITIONS)
            get_target_property(value ${original} ${property})
            if(value)
                set_property(TARGET ${target} PROPERTY ${property} ${value})
            endif()
        endforeach()
    endif()
    target_link_libraries(${target} PUBLIC ${runtime} xxHash::xxhash)
endfunction()
