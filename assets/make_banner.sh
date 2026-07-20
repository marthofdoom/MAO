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

# ── 16:9 tile (1600x900) — same motif, centred composition: title block up
#    top, a larger flask below. For the Nexus mod image / thumbnail.
TW=1600; TH=900
TCX=800; TCY=680; TR=150
TNECK="772,440 828,440 828,535 772,535 772,440"
TCORK="764,422 836,422 836,440 764,440 764,422"
TSURF="656,705 944,705"

magick -size ${TW}x${TH} gradient:'#16181d-#08090b' \
  \( -size ${TW}x${TH} radial-gradient:'#12281f-#000000' -evaluate multiply 0.9 \) \
  -compose screen -composite tbase.png

magick tbase.png \
  \( -clone 0 -fill none -stroke '#4bd8a0' -strokewidth 13 \
     -draw "circle $TCX,$TCY $TCX,$((TCY-TR))" -draw "polyline $TNECK" -blur 0x11 \) \
  -compose screen -composite \
  -stroke none -fill 'rgba(60,180,130,0.16)' \
     -draw "circle $TCX,$TCY $TCX,$((TCY-TR))" -draw "polygon $TNECK" \
  -fill 'rgba(63,214,153,0.34)' -draw "circle $TCX,$((TCY+20)) $TCX,$((TCY+TR-6))" \
  -stroke '#7ef0c0' -strokewidth 2.4 -fill none -draw "line $TSURF" \
  -stroke none -fill 'rgba(190,255,230,0.55)' \
     -draw "circle 764,740 764,745" -draw "circle 838,724 838,728" \
     -draw "circle 802,764 802,769" \
  -fill none -stroke '#e8c87e' -strokewidth 3.0 \
     -draw "circle $TCX,$TCY $TCX,$((TCY-TR))" -draw "polyline $TNECK" \
  -stroke none -fill '#b98a4e' -draw "polygon $TCORK" \
  -fill 'rgba(245,223,168,0.9)' -draw "translate 748,636 rotate 40 rectangle -5,-24 5,24" \
  tflask.png

magick tflask.png -gravity north \
  -font "$P052" \
  -fill '#9a8a5e' -pointsize 40 -kerning 20 -annotate +0+96 "m a r t h" \
  -fill '#eae1cb' -pointsize 132 -kerning 6 -annotate +6+140 "ALCHEMY" \
  -fill '#c9a45c' -pointsize 38 -kerning 30 -annotate +8+300 "O V E R H A U L" \
  -font "$P052I" -fill '#8d939e' -pointsize 27 -kerning 1 -annotate +0+360 \
    "permanent, reusable flasks  —  gather essence, brew once, refill forever" \
  nexus-tile-16x9.png
rm -f tbase.png tflask.png
echo "wrote nexus-tile-16x9.png (${TW}x${TH})"
