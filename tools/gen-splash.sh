#!/bin/sh
# Regenerate the shairkindle e-ink assets. Host-only (ImageMagick, not on the K3).
# Output: extension/share/{splash.png,art-placeholder.png}. Re-run + commit on a
# design change. Panel 600x800; splash occupies y=48..799 (600x752). Grayscale.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out="$here/../extension/share"
mkdir -p "$out"
FONT="${FONT:-/System/Library/Fonts/Avenir Next.ttc}"
[ -f "$FONT" ] || FONT="/System/Library/Fonts/HelveticaNeue.ttc"

# AirPlay glyph as a reusable primitive: rounded-rect "screen" + upward triangle.
glyph() {  # $1=size $2=out
    s="$1"
    magick -size "${s}x${s}" xc:white -fill none -stroke black \
        -draw "stroke-width $((s/28)) roundrectangle $((s*20/100)),$((s*24/100)) $((s*80/100)),$((s*66/100)) $((s/22)),$((s/22))" \
        -stroke none -fill black \
        -draw "polygon $((s*30/100)),$((s*74/100)) $((s*70/100)),$((s*74/100)) $((s*50/100)),$((s*52/100))" \
        -colorspace Gray "$2"
}

# 1) art placeholder — glyph centered on white, 512x512 (matches iOS cover slot).
glyph 512 "$out/art-placeholder.png"

# 2) splash — 600x752 composed layout minus the name (fbink overlays that).
glyph 150 /tmp/sk-glyph.png
magick -size 600x752 xc:white \
    \( /tmp/sk-glyph.png -resize 150x150 \) -gravity North -geometry +0+40 -composite \
    -font "$FONT" -gravity North \
    -fill black  -pointsize 62 -annotate +0+210 "ShairKindle" \
    -fill gray45 -pointsize 24 -annotate +0+298 "AirPlay Receiver" \
    -stroke gray70 -fill none -draw "stroke-width 1 line 220,355 380,355" -stroke none \
    -fill gray45 -pointsize 22 -annotate +0+430 "Select this device on your phone or Mac:" \
    -stroke black -fill none -draw "stroke-width 3 roundrectangle 110,472 490,546 14,14" -stroke none \
    -fill gray55 -pointsize 22 -annotate +0+620 "Waiting for a sender" \
    -colorspace Gray "$out/splash.png"
rm -f /tmp/sk-glyph.png
echo "wrote $out/splash.png $out/art-placeholder.png"
