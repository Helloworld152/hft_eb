#!/bin/bash
cd $(dirname $0)
# 获取当前脚本所在目录（与 run.sh 一致，二进制在 hft_md/bin/）
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
CONFIG_PATH="$SCRIPT_DIR/conf/config.yaml"
RECORDER_BIN="$SCRIPT_DIR/bin/hft_recorder"
CTP_LIB_PATH="$SCRIPT_DIR/../third_party/ctp/lib"

if [ ! -f "$RECORDER_BIN" ]; then
    echo "Error: 未找到 $RECORDER_BIN，请先执行 ./build.sh"
    exit 1
fi
export LD_LIBRARY_PATH=$CTP_LIB_PATH:$LD_LIBRARY_PATH

echo "正在准备夜盘录制配置..."

# 使用固定路径，避免 nohup 子进程启动前被 rm 删掉导致 bad file
NIGHT_CONFIG="$SCRIPT_DIR/conf/config_night.yaml"

# 夜盘数据文件名后缀（仅改文件名，不改 output_path）
NIGHT_FILE_SUFFIX="_night"

# 使用 sed 替换 start_time、end_time 和 file_suffix（不修改 output_path）
sed -E \
    -e 's/^(start_time: ).*/\120:50:00/' \
    -e 's/^(end_time: ).*/\102:40:00/' \
    -e "/^file_suffix: /s/.*/file_suffix: ${NIGHT_FILE_SUFFIX}/" \
    -e "/^output_path: /afile_suffix: ${NIGHT_FILE_SUFFIX}" \
    "$CONFIG_PATH" > "$NIGHT_CONFIG"

echo "夜盘配置: $NIGHT_CONFIG"
echo "启动夜盘录制器..."

nohup "$RECORDER_BIN" "$NIGHT_CONFIG" >> "$SCRIPT_DIR/nohup.out" 2>&1 &
echo "已后台启动，日志: $SCRIPT_DIR/nohup.out"