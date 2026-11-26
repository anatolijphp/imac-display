#!/usr/bin/env bash
cd /root/build
enc(){ ffmpeg -loglevel error -y -f lavfi -i "testsrc2=size=${1}:rate=60,noise=alls=12:allf=t" -t 10 -c:v libx264 -preset veryfast -tune zerolatency -profile:v high -bf 0 -b:v "$2" -maxrate "$2" -bufsize "$2" -pix_fmt yuv420p "$3"; }
[ -f h4k.mp4 ] || enc 3840x2160 60M h4k.mp4
[ -f h1440.mp4 ] || enc 2560x1440 30M h1440.mp4
[ -f h1080.mp4 ] || enc 1920x1080 20M h1080.mp4
for f in h4k h1440 h1080; do ffprobe -v error -show_entries stream=profile,width,height,bit_rate -of compact $f.mp4; done
t(){ local s e; s=$(date +%s.%N); "$@"; e=$(date +%s.%N); echo "$e - $s" | bc; }
fps(){ echo "scale=0; $1/$2" | bc; }
run(){ # label n_parallel file decoder-args...
  local label=$1 n=$2 f=$3; shift 3
  local s e; s=$(date +%s.%N)
  for i in $(seq $n); do ffmpeg -loglevel error "$@" -i $f -f null - & done; wait
  e=$(date +%s.%N); local d=$(echo "$e - $s" | bc)
  echo "$label: $n x 600 frames in $d s -> $(fps 600 $d) fps per stream"
}
VA="-hwaccel vaapi -hwaccel_device /dev/dri/renderD128 -hwaccel_output_format vaapi"
run "4K High, CPU 1 thread"        1 h4k.mp4   -threads 1
run "4K High, CPU 2 threads"       1 h4k.mp4   -threads 2
run "4K High, CPU 4 threads"       1 h4k.mp4   -threads 4
run "4K High, CPU auto threads"    1 h4k.mp4
run "1440p High, CPU 1 thread"     1 h1440.mp4 -threads 1
run "4x1440p High, CPU 1 thr each" 4 h1440.mp4 -threads 1
run "4x1440p High, CPU 2 thr each" 4 h1440.mp4 -threads 2
run "1080p High, VAAPI"            1 h1080.mp4 $VA
run "4x1080p High, VAAPI"          4 h1080.mp4 $VA
run "1080p High, CPU 1 thread"     1 h1080.mp4 -threads 1
