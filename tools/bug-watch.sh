#!/bin/bash
# Usage: tools/bug-watch.sh [session-name]
#
# A screenshot taken on the phone while the game runs becomes a bug report: the
# next Plume frame is traced, and the screenshot, the trace and the tail of the
# runtime log land in out/bugs/<session>/<time>/. A screenshot taken on the PC
# (Win+PrtSc, the reNut PC build as reference) is copied to the same session's
# pc/ folder, so the two can be put side by side. Ctrl+C stops.
cd "$(dirname "$0")/.." || exit 1
ADB=/c/Users/Sidney-PC/AppData/Local/Android/Sdk/platform-tools/adb.exe
export MSYS_NO_PATHCONV=1
SESSION=${1:-$(date +%Y%m%d-%H%M)}
OUT=out/bugs/$SESSION
PC_SHOTS="/c/Users/Sidney-PC/Pictures/Screenshots"
PHONE_SHOTS=/sdcard/DCIM/Screenshots
mkdir -p "$OUT/pc"

list_phone() { $ADB shell "ls $PHONE_SHOTS" 2>/dev/null | tr -d '\r' | grep -v '^thumbnail_' | sort; }
list_pc() { ls "$PC_SHOTS" 2>/dev/null | grep -v desktop.ini | sort; }

seen_phone=$(list_phone)
seen_pc=$(list_pc)
echo "Watching (session $SESSION). Take a screenshot on the phone or on the PC."
while true; do
  now=$(list_phone)
  comm -13 <(echo "$seen_phone") <(echo "$now") | while IFS= read -r shot; do
    [ -n "$shot" ] || continue
    dir="$OUT/$(date +%H%M%S)"
    mkdir -p "$dir"
    $ADB shell "run-as org.renut.android touch files/plume-trace-request"
    for k in $(seq 1 40); do
      $ADB shell "run-as org.renut.android ls files/plume-trace-request" >/dev/null 2>&1 || break
      sleep 0.25
    done
    sleep 3
    $ADB pull "$PHONE_SHOTS/$shot" "$dir/" >/dev/null
    $ADB exec-out "run-as org.renut.android cat files/plume-trace.txt" > "$dir/trace.txt"
    $ADB exec-out "run-as org.renut.android tail -n 400 files/rexglue-runtime.log" > "$dir/runtime-tail.log"
    echo "$(date +%T) phone $shot -> $dir ($(grep -c '^draw #' "$dir/trace.txt") draws)"
  done
  seen_phone=$now
  now=$(list_pc)
  comm -13 <(echo "$seen_pc") <(echo "$now") | while IFS= read -r shot; do
    [ -n "$shot" ] || continue
    cp "$PC_SHOTS/$shot" "$OUT/pc/$(date +%H%M%S)-$shot"
    echo "$(date +%T) pc $shot"
  done
  seen_pc=$now
  sleep 1
done
