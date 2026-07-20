#!/usr/bin/env bash
# 16:9 logo for marth Alchemy Overhaul (1920x1080) — the banner's motif and
# house style (dark ground, essence-green glow, round-bottom flask, gold P052
# type) recomposed for a 16:9 canvas: flask as a centered hero in the upper
# third above stacked, centered typography. Matches the sibling logo-16x9
# format. Use for splash / Nexus header / video thumbnail. Sibling of
# make_banner.sh.
set -euo pipefail
cd "$(dirname "$0")"
W=1920; H=1080
P052=/usr/share/fonts/opentype/urw-base35/P052-Roman.otf
P052I=/usr/share/fonts/opentype/urw-base35/P052-Italic.otf

# Florence flask, centered horizontally, upper third (hero). ~1.8x the banner
# flask, re-centred on cx=960.
CX=960; CY=400; R=150
NECK="934,150 986,150 986,260 934,260 934,150"
CORK="926,132 994,132 994,150 926,150 926,132"
SURF="816,425 1104,425"

# 1) Base: near-black vertical gradient + soft green-alchemical radial glow.
magick -size ${W}x${H} gradient:'#16181d-#08090b' \
  \( -size ${W}x${H} radial-gradient:'#12281f-#000000' -evaluate multiply 0.9 \) \
  -compose screen -composite base.png

# 2) The flask: blurred green glow underlayer, translucent glass, glowing
#    essence body, crisp gold rim + neck + cork, bright glint. Same recipe as
#    the banner, scaled.
magick base.png \
  \( -clone 0 -fill none -stroke '#4bd8a0' -strokewidth 16 \
     -draw "circle $CX,$CY $CX,$((CY-R))" -draw "polyline $NECK" -blur 0x13 \) \
  -compose screen -composite \
  -stroke none -fill 'rgba(60,180,130,0.16)' \
     -draw "circle $CX,$CY $CX,$((CY-R))" -draw "polygon $NECK" \
  -fill 'rgba(63,214,153,0.34)' -draw "circle $CX,$((CY+20)) $CX,$((CY+R-6))" \
  -stroke '#7ef0c0' -strokewidth 2.6 -fill none -draw "line $SURF" \
  -stroke none -fill 'rgba(190,255,230,0.55)' \
     -draw "circle 924,470 924,475" -draw "circle 1002,452 1002,456" \
     -draw "circle 962,498 962,503" \
  -fill none -stroke '#e8c87e' -strokewidth 3.4 \
     -draw "circle $CX,$CY $CX,$((CY-R))" -draw "polyline $NECK" \
  -stroke none -fill '#b98a4e' -draw "polygon $CORK" \
  -fill 'rgba(245,223,168,0.9)' -draw "translate 916,356 rotate 40 rectangle -6,-26 6,26" \
  flask.png

# 3) Typography — centered stack below the flask.
magick flask.png -gravity north \
  -font "$P052" \
  -fill '#9a8a5e' -pointsize 44 -kerning 22 -annotate +0+630 "m a r t h" \
  -fill '#eae1cb' -pointsize 128 -kerning 8 -annotate +0+688 "ALCHEMY" \
  -fill '#c9a45c' -pointsize 40 -kerning 34 -annotate +6+840 "O V E R H A U L" \
  -font "$P052I" -fill '#8d939e' -pointsize 30 -kerning 1 \
  -annotate +0+928 "permanent, reusable flasks  —  gather essence, brew once, refill forever" \
  logo-16x9.png
rm -f base.png flask.png
echo "wrote logo-16x9.png (${W}x${H})"
