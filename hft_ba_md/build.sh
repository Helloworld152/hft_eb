#!/bin/bash
set -e
PROXY_URL="http://192.168.7.9:7890"
export http_proxy="$PROXY_URL"
export https_proxy="$PROXY_URL"
export all_proxy="$PROXY_URL"

mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j4
cd ..
echo "Build finished. Binary is in hft_ba_md/bin/"
