# renut_kernel_overrides(<target>)
#
# The recompiled game calls the kernel's exports by symbol name
# (__imp__<Export>). For each export listed in kernel_overrides.inc, the game's
# code in <target> is compiled with that name replaced by renut_imp_<Export>,
# which kernel_overrides.cpp defines. The SDK is not touched: its own function
# keeps its name and is what an override calls for the cases it does not take.
function(renut_kernel_overrides target)
  file(STRINGS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/kernel_overrides.inc" lines REGEX "^X\\(")
  foreach(line IN LISTS lines)
    string(REGEX REPLACE "^X\\(([A-Za-z0-9_]+)\\).*$" "\\1" name "${line}")
    target_compile_definitions(${target} PRIVATE "__imp__${name}=renut_imp_${name}")
  endforeach()
endfunction()
