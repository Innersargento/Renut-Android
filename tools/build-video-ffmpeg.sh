#!/usr/bin/env bash
set -euo pipefail
export PATH="/usr/bin:/ucrt64/bin:$PATH"
# Windows PowerShell stops on any native stderr line; configure and the
# compiler print harmless warnings there.
exec 2>&1

# Run under MSYS2 bash with the Android NDK path as the first argument and,
# optionally, the output directory (default out/ffmpeg-video). The build is
# out of tree so the source keeps no generated config.h.
root="$(cd "$(dirname "$0")/.." && pwd)"
ndk="$(cygpath -u "${1:?Android NDK path required}")"
video_dir="$(cygpath -u "${2:-$root/out/ffmpeg-video}")"
toolchain="$ndk/toolchains/llvm/prebuilt/windows-x86_64/bin"
source_dir="$video_dir/source"
build_dir="$video_dir/build"
install_dir="$video_dir/install"
rm -rf "$build_dir"
mkdir -p "$build_dir" "$install_dir"
cd "$build_dir"

# NEON stays on: without it the 720p WMV3 decode and the RGBA conversion run
# as plain C on a CPU the game is already saturating.
"$source_dir/configure" \
  --prefix="$install_dir" \
  --target-os=android --arch=aarch64 --cpu=armv8-a --enable-cross-compile \
  --cc="$toolchain/clang.exe --target=aarch64-linux-android28" \
  --cxx="$toolchain/clang++.exe --target=aarch64-linux-android28" \
  --ar="$toolchain/llvm-ar.exe" --ranlib="$toolchain/llvm-ranlib.exe" \
  --nm="$toolchain/llvm-nm.exe" --strip="$toolchain/llvm-strip.exe" \
  --disable-programs --disable-doc --disable-debug --disable-network \
  --disable-everything --disable-autodetect --enable-pic \
  --enable-neon --disable-vulkan \
  --disable-avdevice --disable-avfilter --disable-swresample \
  --enable-avcodec --enable-avformat --enable-avutil --enable-swscale \
  --enable-demuxer=asf --enable-protocol=file \
  --enable-decoder=wmv3 \
  --extra-cflags=-fvisibility=hidden

make -j8
make install
