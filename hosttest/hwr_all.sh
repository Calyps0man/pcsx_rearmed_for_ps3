#!/bin/sh
# record + replay every test program through the hardware renderer; prints worst diff per program
mkdir -p hwr_out
for e in ../testdata/*.exe; do
  b=$(basename $e .exe)
  case $b in CPU*|GTE*) continue;; esac
  ( cd hwr_out && HWR_TRACE=t.tr HWR_EVERY=${EVERY:-40} timeout 600 qemu-ppc64 ../runner_lr_ppc64 ../$e ${FRAMES:-120} pcsx_rearmed_drc=enabled >/dev/null 2>&1
    r=$(GLSL_DIR=../glsl ../hwr_replay t.tr 1 $b 2>&1 | tail -1); rm -f t.tr; echo "$b: $r" )
done
