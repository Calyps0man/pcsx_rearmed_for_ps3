#!/bin/sh
# runall.sh <runner> <outdir> <frames> [opts]  -> screenshot + checksum per test exe
R=$1; O=$2; N=$3; shift 3
mkdir -p $O
for e in ../testdata/*.exe; do
  b=$(basename $e .exe)
  ( cd $O && timeout 300 qemu-ppc64 ../$R ../$e $N $N "$@" > $b.txt 2>&1; [ -f shot_$N.ppm ] && mv shot_$N.ppm $b.ppm )
done
