#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
INPUT_DIR="${ROOT_DIR}/data"
OUTPUT_DIR="${ROOT_DIR}/data/parquet"
BIN_PATH="${SCRIPT_DIR}/target/release/hft_reader"

usage() {
    cat <<USAGE
用法:
  $0                 批量转换 ${INPUT_DIR}/market_data_*.dat
  $0 <YYYYMMDD>      仅转换指定日期（market_data_<YYYYMMDD>.dat）
  $0 --all           批量转换全部

示例:
  $0
  $0 20260210
  $0 --all
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

mkdir -p "${OUTPUT_DIR}"

cd "${SCRIPT_DIR}"
echo "[1/3] 编译 release 版本..."
cargo build --release

convert_one() {
    local base_path="$1"
    local file_name
    local out_path

    file_name="$(basename "${base_path}")"
    out_path="${OUTPUT_DIR}/${file_name}.parquet"

    echo "转换: ${base_path}.dat -> ${out_path}"
    "${BIN_PATH}" "${base_path}" --parquet -o "${out_path}"
}

declare -a targets=()

if [[ -n "${1:-}" && "${1}" != "--all" ]]; then
    DATE="$1"
    targets+=("${INPUT_DIR}/market_data_${DATE}")
else
    while IFS= read -r dat_file; do
        targets+=("${dat_file%.dat}")
    done < <(find "${INPUT_DIR}" -maxdepth 1 -type f -name 'market_data_*.dat' | sort)
fi

if [[ "${#targets[@]}" -eq 0 ]]; then
    echo "未找到可转换文件: ${INPUT_DIR}/market_data_*.dat"
    exit 1
fi

echo "[2/3] 待转换文件数: ${#targets[@]}"

ok=0
fail=0
for base in "${targets[@]}"; do
    if [[ ! -f "${base}.dat" ]]; then
        echo "跳过: 不存在 ${base}.dat"
        ((fail+=1))
        continue
    fi

    if convert_one "${base}"; then
        ((ok+=1))
    else
        echo "失败: ${base}.dat"
        ((fail+=1))
    fi
done

echo "[3/3] 完成: 成功 ${ok}，失败 ${fail}"

if [[ "${fail}" -gt 0 ]]; then
    exit 1
fi
