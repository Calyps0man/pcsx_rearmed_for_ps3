#!/bin/sh
# native VRAM of the full-mode GL model vs the software renderer's VRAM at frame N
# (colour and bit-15 differences), per test program
N=${FRAMES:-120}; O=${OUT:-vram_out}; mkdir -p $O
for e in ../testdata/*.exe; do
  b=$(basename $e .exe)
  case $b in CPU*|GTE*) continue;; esac
  ( cd $O && VRAM_DUMP=1 HWR_GL=${SCALE:-1} GLSL_DIR=${GLSL:-../glsl} timeout 300 ../runner_gl ../$e $N $N >/dev/null 2>$b.log
    mv -f vram_$N.bin $b.gl.bin 2>/dev/null; VRAM_DUMP=1 timeout 300 ../runner_gl ../$e $N $N >/dev/null 2>&1; mv -f vram_$N.bin $b.sw.bin 2>/dev/null
    if [ -f $b.gl.bin ] && [ -f $b.sw.bin ]; then echo "$b: $(python3 ../cmp_vram.py $b.sw.bin $b.gl.bin)"; else echo "$b: no dump"; fi )
done
