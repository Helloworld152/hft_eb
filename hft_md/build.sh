#!/bin/bash
mkdir -p build
cd build
cmake ..
make -j4
cd ..
echo "Build finished. Binary is in hft_md/bin/"
