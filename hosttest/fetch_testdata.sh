#!/bin/sh
# Downloads the PS1 test programs used by the host tests (PeterLemon/PSX, by krom)
set -e
cd "$(dirname "$0")/.."
rm -rf .psx-src && git clone -q --depth 1 https://github.com/PeterLemon/PSX .psx-src
mkdir -p testdata/ref
find .psx-src -iname '*.exe' -exec cp {} testdata/ \;
find .psx-src/GPU -iname '*.png' -exec cp {} testdata/ref/ \;
rm -rf .psx-src
ls testdata/*.exe | wc -l
