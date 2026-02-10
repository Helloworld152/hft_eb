#!/bin/bash
# 参考 run.sh 的执行逻辑
cd bin
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:.
./hft_engine ../conf/config_kline_synth.yaml