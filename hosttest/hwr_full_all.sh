#!/bin/sh
# full-mode renderer (hwr_gl.c, native build): the GPU draws everything; compares the
# displayed picture at frame N with the software renderer's, per test program
N=${FRAMES:-120}; O=${OUT:-full_out}; mkdir -p $O
for e in ../testdata/*.exe; do
  b=$(basename $e .exe)
  case $b in CPU*|GTE*) continue;; esac
  ( cd $O && HWR_GL=${SCALE:-1} GLSL_DIR=${GLSL:-../glsl} timeout 300 ../runner_gl ../$e $N $N >/dev/null 2>$b.log
    mv -f gl_$N.ppm $b.gl.ppm 2>/dev/null; timeout 300 ../runner_gl ../$e $N $N >/dev/null 2>&1; mv -f shot_$N.ppm $b.sw.ppm 2>/dev/null
    if [ -f $b.gl.ppm ] && [ -f $b.sw.ppm ]; then echo "$b: $(python3 ../cmp.py $b.sw.ppm $b.gl.ppm | sed 's/.*: //') ($(grep -o 'prims [0-9]*\|syncs in [0-9]* out [0-9]*' $b.log | tr '\n' ' '))"
    else echo "$b: no picture ($(tail -1 $b.log))"; fi )
done
