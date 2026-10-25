#!/bin/sh
# Regenerates installer/msix/Assets/*.png from assets/winfreezetrace.ico (needs ffmpeg).
set -e
cd "$(dirname "$0")"
SRC=../../assets/winfreezetrace.ico
OUT=Assets
mkdir -p "$OUT"
ffmpeg -v error -y -i "$SRC" -map 0:v:7 -frames:v 1 "$OUT/_src.png"
sq() { # name size
  ffmpeg -v error -y -i "$OUT/_src.png" -vf "scale=$2:$2:flags=lanczos,format=rgba" "$OUT/$1.png"
}
wide() { # name w h icon
  ffmpeg -v error -y -i "$OUT/_src.png" -vf "scale=-1:$4:flags=lanczos,format=rgba,pad=$2:$3:(ow-iw)/2:(oh-ih)/2:color=0x00000000" "$OUT/$1.png"
}
for s in 100:1 125:1.25 150:1.5 200:2 400:4; do
  sc=${s%%:*}; m=${s##*:}
  sq "Square44x44Logo.scale-$sc"  "$(awk "BEGIN{print int(44*$m+.5)}")"
  sq "Square71x71Logo.scale-$sc"  "$(awk "BEGIN{print int(71*$m+.5)}")"
  sq "Square150x150Logo.scale-$sc" "$(awk "BEGIN{print int(150*$m+.5)}")"
  sq "Square310x310Logo.scale-$sc" "$(awk "BEGIN{print int(310*$m+.5)}")"
  sq "StoreLogo.scale-$sc"        "$(awk "BEGIN{print int(50*$m+.5)}")"
  wide "Wide310x150Logo.scale-$sc" "$(awk "BEGIN{print int(310*$m+.5)}")" "$(awk "BEGIN{print int(150*$m+.5)}")" "$(awk "BEGIN{print int(150*$m+.5)}")"
done
for t in 16 24 32 48 256; do
  sq "Square44x44Logo.targetsize-$t" $t
  sq "Square44x44Logo.targetsize-${t}_altform-unplated" $t
done
for n in Square44x44Logo Square71x71Logo Square150x150Logo Square310x310Logo StoreLogo Wide310x150Logo; do cp "$OUT/$n.scale-100.png" "$OUT/$n.png"; done
rm "$OUT/_src.png"
