#!/bin/bash
set -e

# 创建构建目录 (build for intermediate files)
BUILD_DIR="build"

if [ "$1" == "clean" ]; then
    echo ">>> Cleaning build directory..."
    rm -rf $BUILD_DIR
fi

if [ ! -d "$BUILD_DIR" ]; then
    mkdir -p $BUILD_DIR
fi

# 创建输出目录 (bin for executables/libs)
BIN_DIR="bin"
mkdir -p $BIN_DIR

echo ">>> Starting Release Build..."

# 进入 build 目录并执行 CMake
cd $BUILD_DIR
cmake -DCMAKE_BUILD_TYPE=Release ..

# 编译
make -j$(nproc)

echo ">>> Build Complete. Binary is at $BIN_DIR/hft_engine"