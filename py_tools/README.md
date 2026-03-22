# HFT Tools Manual

### 1. 转换 (Binary -> Parquet)

- **工具**: `rust_tools/run_convert.sh`
- **功能**: 使用 Rust 将原始 `.dat` 转为数字协议 Parquet。
- **用法**: `cd rust_tools && ./run_convert.sh 20260210`
- **存储**: `data/parquet/`

### 2. 聚合 (Tick -> Kline)

- **工具**: `tools/gen_kline.py`
- **功能**: 将 Tick 数据聚合成指定周期的 K 线。
- **用法**: `python3 tools/gen_kline.py data/parquet/xxx.parquet 1m`
- **存储**: `data/kline/`

---

### 数字协议 (Numeric Protocol)


| ID  | 字段       | ID  | 字段              |
| --- | -------- | --- | --------------- |
| 1   | Symbol   | 5   | LastPrice       |
| 2   | SymbolID | 6   | Volume          |
| 3   | Day      | 15  | BidPrice (List) |
| 4   | Time     | 17  | AskPrice (List) |


