#!/bin/bash
set -e
cd "$(dirname "$0")" || exit
#rm -rf build
mkdir -p build
cd build || exit
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j"$(nproc)"
exec ./crossfade
