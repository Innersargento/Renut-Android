#!/usr/bin/env bash
# Recompile Banjo-Kazooie: Nuts & Bolts shaders to SPIR-V for the native Plume
# renderer, with masterspike52/reNut-XenosRecomp plus renut-xenosrecomp.patch.
# Runs in WSL (Ubuntu with clang, cmake, ninja; spirv-tools for the check).
#
#   tools/shaders/recompile_shaders.sh <input dir> <out dir>
#
# <input dir> holds files with complete shader containers (header followed by
# microcode): the decrypted xex.bin and the runtime capture (files/shader-dump
# on the device, written by android/rexglue/d3d_layer.cpp). Do not point
# it at the extracted game data - the CAFF bundles store only the headers of
# their shaders (the microcode lives elsewhere), and XenosRecomp would turn
# whatever bytes follow each header into a shader that compiles but is garbage.
# See docs/SHADERS-PLUME.md.
set -euo pipefail

INPUT=$1
OUT_DIR=$2

HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${XENOS_RECOMP_WORK:-$HOME/rxr}
FORK_COMMIT=8f781f8e5f06e0ddd9c03c8155891af8f5c3786c

if [ ! -d "$WORK/.git" ]; then
    git clone --recursive https://github.com/masterspike52/reNut-XenosRecomp "$WORK"
fi

cd "$WORK"
git checkout --quiet "$FORK_COMMIT"
git checkout --quiet -- .
git apply "$HERE/renut-xenosrecomp.patch"

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ > /dev/null
cmake --build build -j"$(nproc)"

mkdir -p "$OUT_DIR"
# The fifth argument lists the vertex shaders to recompile a second time as the
# domain stage of a tessellated draw - see tools/shaders/domain-shaders.txt.
./build/XenosRecomp/XenosRecomp "$INPUT" "$OUT_DIR/shader_cache.cpp" \
    XenosRecomp/shader_common.h "$OUT_DIR/hlsl" "$HERE/domain-shaders.txt" 2>&1 | tee "$OUT_DIR/recompile.log"
cp XenosRecomp/shader_common.h "$OUT_DIR/shader_common.h"

# Decode every entry and validate its SPIR-V.
cp "$HERE/../../android/rexglue/shader_cache.h" "$OUT_DIR/"
clang++ -std=c++17 -O1 -I"$OUT_DIR" -Ithirdparty/smol-v/source -Ithirdparty/zstd/lib \
    "$HERE/verify_cache.cpp" "$OUT_DIR/shader_cache.cpp" thirdparty/smol-v/source/smolv.cpp \
    build/thirdparty/zstd/build/cmake/lib/libzstd.a -o "$OUT_DIR/verify"
rm -rf "$OUT_DIR/spv" && mkdir "$OUT_DIR/spv"
"$OUT_DIR/verify" "$OUT_DIR/spv" "$OUT_DIR/shader_spirv.smolv"
failures=0
for spv in "$OUT_DIR"/spv/*.spv; do
    spirv-val --target-env vulkan1.1 "$spv" > /dev/null 2>&1 || failures=$((failures + 1))
done
echo "spirv-val failures: $failures"
rm -f "$OUT_DIR/shader_cache.h"
[ "$failures" -eq 0 ]

# What the Android build consumes (android/rexglue/shader_cache.h):
#   shader_cache.cpp      the entry and input-element tables, without the
#                         compressed blob - the decompressed smol-v blob is
#                         embedded with .incbin instead (no zstd on device)
#   shader_spirv.smolv    that blob
#   shader_ucode_map.inc  microcode hash -> container hash, for the command
#                         processor, which only ever sees the microcode
grep -v -e "^const uint8_t g_compressedSpirvCache" -e "g_spirvCacheCompressedSize" \
    -e "g_spirvCacheDecompressedSize" "$OUT_DIR/shader_cache.cpp" > "$OUT_DIR/shader_cache.tmp"
mv "$OUT_DIR/shader_cache.tmp" "$OUT_DIR/shader_cache.cpp"
clang++ -std=c++17 -O2 -Ithirdparty/xxHash -I"$HERE/../../android/rexglue" "$HERE/ucode_map.cpp" thirdparty/xxHash/xxhash.c -o "$OUT_DIR/ucode_map"
"$OUT_DIR/ucode_map" "$INPUT" > "$OUT_DIR/shader_ucode_map.inc"
