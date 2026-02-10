#!/bin/bash

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"

if [ "$1" == "" ]; then
    echo "用法: $0 <文件名(不带后缀)>"
    exit 1
fi

"${SCRIPT_DIR}/bin/hft_reader" "$1"
