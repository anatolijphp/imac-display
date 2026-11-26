#!/usr/bin/env bash
# 3840x2160 panel test pattern: red border, grid, circle (aspect), 1-pixel stripes (sharpness), corner labels.
set -e
F=/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf
border='lt(X,8)+gt(X,W-9)+lt(Y,8)+gt(Y,H-9)'
ring='lt(abs(hypot(X-W/2,Y-H/2)-900),4)'
grid='lt(mod(X,240),2)+lt(mod(Y,240),2)'
inx='between(X,400,999)'
vstr="$inx*between(Y,400,799)*mod(X,2)"
hstr="$inx*between(Y,900,1299)*mod(Y,2)"
chk="$inx*between(Y,1400,1799)*mod(floor(X/2)+floor(Y/2),2)"
fine="($vstr+$hstr+$chk)"
patch="($inx*(between(Y,400,799)+between(Y,900,1299)+between(Y,1400,1799)))"
# value: border red; ring white; fine patterns white/black; grid grey; background dark
R="if($border,255,if($ring,255,if($patch,255*$fine,if($grid,110,30))))"
G="if($border,0,if($ring,255,if($patch,255*$fine,if($grid,110,30))))"
B="if($border,0,if($ring,255,if($patch,255*$fine,if($grid,110,30))))"
txt(){ echo "drawtext=fontfile=$F:fontcolor=white:fontsize=$1:x=$2:y=$3:text='$4'"; }
ffmpeg -loglevel error -y -f lavfi -i color=c=black:s=3840x2160 -frames:v 1 -vf "format=rgb24,geq=r='$R':g='$G':b='$B',\
$(txt 64 40 40 'TOP LEFT'),$(txt 64 w-tw-40 40 'TOP RIGHT'),$(txt 64 40 h-th-40 'BOTTOM LEFT'),$(txt 64 w-tw-40 h-th-40 'BOTTOM RIGHT'),\
$(txt 48 1040 560 'A  1-pixel vertical lines'),$(txt 48 1040 1060 'B  1-pixel horizontal lines'),$(txt 48 1040 1560 'C  2-pixel checkerboard'),\
$(txt 96 '(w-tw)/2' '(h-th)/2-60' '3840 x 2160'),$(txt 48 '(w-tw)/2' '(h/2)+80' 'the circle should be round')" /root/build/pattern.png
