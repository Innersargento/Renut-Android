#!/usr/bin/env bash
# Decompress the XCTD files (magic 0FF512ED) of Bundle/4f into plain CAFFs, for
# tools/shaders/extract_caff_shaders.py. Needs a C compiler (clang) and the
# libmspack sources that tools/prepare.py deps fetches.
#
#   tools/shaders/decompress_xctd.sh <game Bundle/4f dir> <out dir>
#
# XCTD is the console's transparent file compression: LZX in independent
# segments, which xctd_decompress.c reads (the format is described there).
# This replaces an earlier QuickBMS step that only ran on Windows.
set -euo pipefail

IN=$1
OUT=$2

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
MSPACK=${MSPACK_DIR:-$ROOT/thirdparty/rexglue-sdk/thirdparty/libmspack/libmspack/mspack}
BIN=${XCTD_BIN:-$ROOT/out/prepare/xctd_decompress}
CC_BIN=${CC:-$(command -v clang || command -v cc || command -v gcc)}

if [ ! -f "$MSPACK/lzxd.c" ]; then
    echo "libmspack not found at $MSPACK (run tools/prepare.py deps)" >&2
    exit 1
fi
if [ ! -x "$BIN" ] || [ "$HERE/xctd_decompress.c" -nt "$BIN" ]; then
    mkdir -p "$(dirname "$BIN")"
    "$CC_BIN" -O2 -I"$MSPACK" "$HERE/xctd_decompress.c" "$MSPACK/lzxd.c" -o "$BIN"
fi

mkdir -p "$OUT"
export BIN OUT

one() {
    f=$1
    name=$(basename "$f")
    # Only XCTD files; the rest of the folder is not ours to touch.
    [ "$(head -c4 "$f" | od -An -tx1 | tr -d ' \n')" = "0ff512ed" ] || return 0
    [ -s "$OUT/$name" ] && return 0
    if "$BIN" "$f" "$OUT/$name.part" && [ "$(head -c4 "$OUT/$name.part")" = "CAFF" ]; then
        mv "$OUT/$name.part" "$OUT/$name"
    else
        rm -f "$OUT/$name.part"
        echo "failed: $name" >&2
        return 1
    fi
}
export -f one

JOBS=$(nproc 2>/dev/null || echo 2)
if ! find "$IN" -maxdepth 1 -type f -print0 | xargs -0 -n1 -P"$JOBS" bash -c 'one "$0"'; then
    echo "some XCTD files could not be decompressed" >&2
    exit 1
fi
echo "decompressed $(find "$OUT" -maxdepth 1 -type f ! -name '*.part' | wc -l) files into $OUT"
