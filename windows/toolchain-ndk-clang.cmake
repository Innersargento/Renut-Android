# Windows x86-64 toolchain for reNut's Plume experiment without Visual Studio:
# the Android NDK's clang (a Windows-hosted clang 21) targeting MinGW-w64, with
# the MSYS2 UCRT64 headers, libstdc++ and CRT as its sysroot.
#
# cmake -S windows -B out/build/win-plume -G Ninja -DCMAKE_TOOLCHAIN_FILE=windows/toolchain-ndk-clang.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT DEFINED ENV{RENUT_NDK_BIN})
    file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}" _local_app_data)
    file(GLOB _ndk_dirs "${_local_app_data}/Android/Sdk/ndk/*/toolchains/llvm/prebuilt/windows-x86_64/bin")
    list(SORT _ndk_dirs)
    list(REVERSE _ndk_dirs)
    list(GET _ndk_dirs 0 _ndk_bin)
else()
    set(_ndk_bin "$ENV{RENUT_NDK_BIN}")
endif()
set(RENUT_MSYS "C:/msys64/ucrt64" CACHE PATH "MSYS2 UCRT64 prefix (headers, libstdc++, CRT)")

set(CMAKE_C_COMPILER "${_ndk_bin}/clang.exe")
set(CMAKE_CXX_COMPILER "${_ndk_bin}/clang++.exe")
set(CMAKE_ASM_COMPILER "${_ndk_bin}/clang.exe")
set(CMAKE_AR "${_ndk_bin}/llvm-ar.exe")
set(CMAKE_RANLIB "${_ndk_bin}/llvm-ranlib.exe")
set(CMAKE_RC_COMPILER "${_ndk_bin}/llvm-windres.exe")
set(CMAKE_C_COMPILER_TARGET x86_64-w64-windows-gnu)
set(CMAKE_CXX_COMPILER_TARGET x86_64-w64-windows-gnu)
set(CMAKE_ASM_COMPILER_TARGET x86_64-w64-windows-gnu)

# SSE4.1 is the floor the SDK's own helper sets; -fms-extensions is what lets the
# SDK's Windows sources (__try/__except, GetProcAddress into a void*) compile
# with a GNU-driver clang, as they do under clang-cl.
set(_common "--sysroot=${RENUT_MSYS} -msse4.1 -fms-extensions")
set(CMAKE_C_FLAGS_INIT "${_common}")
set(CMAKE_CXX_FLAGS_INIT "${_common}")
set(CMAKE_ASM_FLAGS_INIT "${_common}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld -static")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
