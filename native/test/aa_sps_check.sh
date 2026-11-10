#!/bin/sh
# Host check of the SPS cropping rewrite on real encoder output: ffmpeg/libx264 streams of
# 1280x720 (High with and without scaling matrices, Main, Baseline), each SPS rewritten by
# aa-check, and ffprobe must then report the cluster viewport, 1010x376.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../build/check"
CHECK="$OUT/aa-check"
FFMPEG="${FFMPEG:-ffmpeg}"
FFPROBE="${FFPROBE:-ffprobe}"
command -v "$FFMPEG" > /dev/null || { echo "aa sps: ffmpeg not found, skipped"; exit 0; }
for variant in "high:-profile:v high" "high-cqm:-profile:v high -x264-params cqm=jvt" \
               "main:-profile:v main" "baseline:-profile:v baseline"; do
    name="${variant%%:*}"
    opts="${variant#*:}"
    "$FFMPEG" -v error -y -f lavfi -i "testsrc2=size=1280x720:rate=25" -frames:v 6 \
        -c:v libx264 $opts -pix_fmt yuv420p -g 3 -f h264 "$OUT/sps-$name.h264"
    "$CHECK" sps "$OUT/sps-$name.h264" "$OUT/sps-$name-crop.h264"
    size="$("$FFPROBE" -v error -select_streams v:0 -show_entries stream=width,height \
        -of csv=p=0 "$OUT/sps-$name-crop.h264")"
    frames="$("$FFPROBE" -v error -select_streams v:0 -count_frames -show_entries \
        stream=nb_read_frames -of csv=p=0 "$OUT/sps-$name-crop.h264")"
    if [ "$size" != "1010,376" ] || [ "$frames" != "6" ]; then
        echo "aa sps ($name): FAILED, got ${size} ${frames} frames"
        exit 1
    fi
    echo "aa sps ($name): 1010x376, $frames frames decoded"
done
