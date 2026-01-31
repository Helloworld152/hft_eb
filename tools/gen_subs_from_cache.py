import json
import yaml
import sys

ins_file = "conf/latest_ins_cache.json"
symbols_file = "conf/symbols.txt"
config_file = "hft_md/conf/config.yaml"

print(f"Loading {ins_file}...")

try:
    with open(ins_file, 'r', encoding='utf-8') as f:
        data = json.load(f)
except Exception as e:
    print(f"Error loading JSON: {e}")
    sys.exit(1)

print(f"Total entries: {len(data)}")

contracts = []
skipped_expired = 0
skipped_type = 0

for key, val in data.items():
    # 1. 过滤过期
    if val.get('expired', True): 
        skipped_expired += 1
        continue
    
    # 2. 过滤类型 (只保留期货 FUTURE)
    # 快期分类: FUTURE, FUTURE_INDEX, FUTURE_OPTION, COMBINE
    cls = val.get('class', '')
    if cls != 'FUTURE':
        skipped_type += 1
        continue

    # 3. 提取 CTP InstrumentID
    # 快期格式: CFFEX.IF2406 -> CTP: IF2406
    # 也有可能是 DCE.m2409 -> m2409
    raw_id = val.get('instrument_id', '')
    
    if '.' in raw_id:
        parts = raw_id.split('.')
        ctp_id = parts[-1]
    else:
        ctp_id = raw_id

    # 简单验证: CTP 合约通常包含数字
    if not any(char.isdigit() for char in ctp_id):
        continue

    contracts.append(ctp_id)

# 去重并排序
contracts = sorted(list(set(contracts)))

print(f"Selected {len(contracts)} contracts (Skipped: {skipped_expired} expired, {skipped_type} non-future)")

if len(contracts) == 0:
    print("No contracts found! Check filters.")
    sys.exit(1)

# --- Step 1: Write symbols.txt ---
with open(symbols_file, "w") as f:
    start_id = 10000001
    for i, c in enumerate(contracts):
        f.write(f"{start_id + i}:{c}\n")
print(f"Updated {symbols_file}")

# --- Step 2: Update config.yaml ---
with open(config_file, 'r') as f:
    config = yaml.safe_load(f)

config['symbols'] = contracts

# 强制 trading_day 20260131 (或者保留原样)
# config['trading_day'] = 20260131 

with open(config_file, 'w') as f:
    yaml.dump(config, f, default_flow_style=False, sort_keys=False)

print(f"Updated {config_file}")
