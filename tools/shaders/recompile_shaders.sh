#!/usr/bin/env bash
# Recompile Banjo-Kazooie: Nuts & Bolts shaders to SPIR-V for the native Plume
# renderer, with thirdparty/XenosRecomp (masterspike52/reNut-XenosRecomp, patched).
# Runs in WSL (Ubuntu with clang, cmake, ninja; spirv-tools for the check).
#
#   tools/shaders/recompile_shaders.sh <input dir> <out dir>
#
# <input dir> holds files with complete shader containers (header followed by
# microcode): the decrypted xex.bin and the runtime capture (files/shader-dump
# on the device, written by src/d3d/d3d_layer.cpp), and the output of
# tools/shaders/extract_caff_shaders.py. Do not point it at the game data
# itself - the CAFF bundles keep a shader's header apart from its microcode,
# and XenosRecomp would turn whatever bytes follow each header into a shader
# that compiles but is garbage. See docs/SHADERS-PLUME.md.
set -euo pipefail

# --prepare-only: build the recompiler, then stop (tools/prepare.py deps).
PREPARE_ONLY=0
if [ "${1:-}" = "--prepare-only" ]; then
    PREPARE_ONLY=1
else
    INPUT=$1
    OUT_DIR=$2
fi

HERE=$(cd "$(dirname "$0")" && pwd)
# The recompiler is thirdparty/XenosRecomp: masterspike52/reNut-XenosRecomp at 8f781f8 with
# renut-xenosrecomp.patch already applied (see thirdparty/XenosRecomp/RENUT.md). It is built from a copy
# on the Linux file system - compiling and loading libdxcompiler.so from /mnt/c is slow - kept up to
# date with the repository's, so only what changed is built again.
REPO=$(cd "$HERE/../.." && pwd)
WORK=${XENOS_RECOMP_WORK:-$HOME/.cache/renut-shader-tool}
SRC=$WORK/src
BUILD=$WORK/build

mkdir -p "$SRC"
cp -au "$REPO/thirdparty/XenosRecomp/." "$SRC/"
cd "$SRC"

cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ > /dev/null
cmake --build "$BUILD" -j"$(nproc)"
[ "$PREPARE_ONLY" = 1 ] && exit 0

mkdir -p "$OUT_DIR"
# The fifth argument lists the vertex shaders to recompile a second time as the
# domain stage of a tessellated draw - see tools/shaders/domain-shaders.txt.
# DOMAIN_SHADERS replaces that list (tools/prepare.py adds the shaders a device log named).
"$BUILD/XenosRecomp/XenosRecomp" "$INPUT" "$OUT_DIR/shader_cache.cpp" \
    XenosRecomp/shader_common.h "$OUT_DIR/hlsl" "${DOMAIN_SHADERS:-$HERE/domain-shaders.txt}" 2>&1 | tee "$OUT_DIR/recompile.log"
cp XenosRecomp/shader_common.h "$OUT_DIR/shader_common.h"

# Decode every entry and validate its SPIR-V.
cp "$HERE/../../src/plume/shader_cache.h" "$OUT_DIR/"
clang++ -std=c++17 -O1 -I"$OUT_DIR" -Ithirdparty/smol-v/source -Ithirdparty/zstd/lib \
    "$HERE/verify_cache.cpp" "$OUT_DIR/shader_cache.cpp" thirdparty/smol-v/source/smolv.cpp \
    "$BUILD/thirdparty/zstd/build/cmake/lib/libzstd.a" -o "$OUT_DIR/verify"
rm -rf "$OUT_DIR/spv" && mkdir "$OUT_DIR/spv"
"$OUT_DIR/verify" "$OUT_DIR/spv" "$OUT_DIR/shader_spirv.smolv"
failures=0
for spv in "$OUT_DIR"/spv/*.spv; do
    spirv-val --target-env vulkan1.1 "$spv" > /dev/null 2>&1 || failures=$((failures + 1))
done
echo "spirv-val failures: $failures"
rm -f "$OUT_DIR/shader_cache.h"
[ "$failures" -eq 0 ]

# What the Android build consumes (src/plume/shader_cache.h):
#   shader_cache.cpp      the entry and input-element tables, without the
#                         compressed blob - the decompressed smol-v blob is
#                         embedded with .incbin instead (no zstd on device)
#   shader_spirv.smolv    that blob
#   shader_ucode_map.inc  microcode hash -> container hash, for the command
#                         processor, which only ever sees the microcode
grep -v -e "^const uint8_t g_compressedSpirvCache" -e "g_spirvCacheCompressedSize" \
    -e "g_spirvCacheDecompressedSize" "$OUT_DIR/shader_cache.cpp" > "$OUT_DIR/shader_cache.tmp"
mv "$OUT_DIR/shader_cache.tmp" "$OUT_DIR/shader_cache.cpp"
clang++ -std=c++17 -O2 -Ithirdparty/xxHash -I"$HERE/../../src/plume" "$HERE/ucode_map.cpp" thirdparty/xxHash/xxhash.c -o "$OUT_DIR/ucode_map"
"$OUT_DIR/ucode_map" "$INPUT" > "$OUT_DIR/shader_ucode_map.inc"
