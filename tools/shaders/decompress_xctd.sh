#!/usr/bin/env bash
# Decompress the XCTD files (magic 0FF512ED) of Bundle/4f into plain CAFFs, for
# tools/shaders/extract_caff_shaders.py. Runs in WSL.
#
#   tools/shaders/decompress_xctd.sh <game Bundle/4f dir> <out dir>
#
# XCTD is xcompress LZX decoded segment by segment with XMemDecompressSegmentTD,
# which only exists in Microsoft's xcompress library. QuickBMS's Windows build
# links it (its Linux build does not), so this runs quickbms.exe through WSL
# interop, from https://aluigi.altervista.org/papers/quickbms.zip unpacked
# into $QUICKBMS_DIR (default ~/qbms/win).
set -euo pipefail

IN=$1
OUT=$2
QBMS=${QUICKBMS_DIR:-$HOME/qbms/win}/quickbms_4gb_files.exe

mkdir -p "$OUT"
script=$(mktemp --suffix=.bms)
printf 'comtype xmemdecompress\nget ZSIZE asize\nclog "" 0 ZSIZE ZSIZE\n' > "$script"

ok=0
bad=0
for f in "$IN"/*; do
    name=$(basename "$f")
    [ "$(head -c4 "$f" | xxd -p)" = "0ff512ed" ] || continue
    [ -s "$OUT/$name" ] && { ok=$((ok + 1)); continue; }
    tmp=$(mktemp -d)
    "$QBMS" -Q -o "$(wslpath -w "$script")" "$(wslpath -w "$f")" "$(wslpath -w "$tmp")" > /dev/null 2>&1 || true
    result=$(ls "$tmp" | head -1)
    if [ -n "$result" ] && [ "$(head -c4 "$tmp/$result")" = "CAFF" ]; then
        mv "$tmp/$result" "$OUT/$name"
        ok=$((ok + 1))
    else
        bad=$((bad + 1))
        echo "failed: $name"
    fi
    rm -rf "$tmp"
done
rm -f "$script"
echo "decompressed $ok, failed $bad"
[ "$bad" -eq 0 ]
