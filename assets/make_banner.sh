#!/usr/bin/env bash
# Nexus banner for marth Alchemy Overhaul (1300x372).
# Motif: a round-bottom alchemist's flask with glowing green essence — gather,
# brew once, refill forever. House style matches the sibling overhauls (dark
# ground, gold type, P052): only the motif + accent (amethyst -> essence green)
# and the copy differ.
set -euo pipefail
cd "$(dirname "$0")"
W=1300; H=372
P052=/usr/share/fonts/opentype/urw-base35/P052-Roman.otf
P052I=/usr/share/fonts/opentype/urw-base35/P052-Italic.otf

# Florence flask (round bulb + narrow neck + cork), sat on the right.
CX=1092; CY=224; R=74            # bulb: centre + radius
NECK="1078,96 1106,96 1106,158 1078,158 1078,96"   # neck rectangle
CORK="1072,80 1112,80 1112,96 1072,96 1072,80"     # stopper
# Liquid surface line across the bulb (~55% full) + a couple of rising bubbles.
SURF="1024,238 1160,238"

# 1) Base: near-black vertical gradient + soft green-alchemical radial glow.
magick -size ${W}x${H} gradient:'#16181d-#08090b' \
  \( -size ${W}x${H} radial-gradient:'#12281f-#000000' -evaluate multiply 0.9 \) \
  -compose screen -composite base.png

# 2) The flask: a blurred green glow underlayer, translucent glass fill, the
#    glowing essence body, a crisp gold rim + neck + cork, and a bright glint.
magick base.png \
  \( -clone 0 -fill none -stroke '#4bd8a0' -strokewidth 11 \
     -draw "circle $CX,$CY $CX,$((CY-R))" -draw "polyline $NECK" -blur 0x9 \) \
  -compose screen -composite \
  -stroke none -fill 'rgba(60,180,130,0.16)' \
  -draw "circle $CX,$CY $CX,$((CY-R))" -draw "polygon $NECK" \
  -fill 'rgba(63,214,153,0.34)' -draw "circle $CX,$((CY+18)) $CX,$((CY+R-4))" \
  -stroke '#7ef0c0' -strokewidth 2.0 -fill none -draw "line $SURF" \
  -stroke none -fill 'rgba(190,255,230,0.55)' \
     -draw "circle 1070,262 1070,266" -draw "circle 1108,250 1108,253" \
     -draw "circle 1088,278 1088,282" \
  -fill none -stroke '#e8c87e' -strokewidth 2.6 \
     -draw "circle $CX,$CY $CX,$((CY-R))" -draw "polyline $NECK" \
  -stroke '#c9a45c' -strokewidth 1.6 -draw "line 1078,150 1106,150" \
  -stroke none -fill '#b98a4e' -draw "polygon $CORK" \
  -fill 'rgba(245,223,168,0.9)' -draw "translate 1058,196 rotate 40 rectangle -4,-16 4,16" \
  flask.png

# 3) Typography (left-aligned so it clears the flask).
magick flask.png \
  -font "$P052" -gravity northwest \
  -fill '#9a8a5e' -pointsize 30 -kerning 14 -annotate +72+58 "m a r t h" \
  -fill '#eae1cb' -pointsize 84 -kerning 5 -annotate +68+92 "ALCHEMY" \
  -fill '#c9a45c' -pointsize 26 -kerning 21 -annotate +76+206 "O V E R H A U L" \
  -font "$P052I" -fill '#8d939e' -pointsize 22 -kerning 1 \
  -annotate +74+270 "permanent, reusable flasks  —  gather essence, brew once, refill forever" \
  nexus-banner.png
rm -f base.png flask.png
echo "wrote nexus-banner.png (${W}x${H})"
