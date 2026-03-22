import json
import os
import sys
import yaml

ins_file = "/tmp/latest_ins_cache.json"
symbols_file = "conf/symbols.txt"
config_file = "hft_md/conf/config.yaml"

# 先从 txt 读取已有 symbol -> id，保证同合约 id 确定性
symbol_to_id = {}
max_id = 10000000
if os.path.exists(symbols_file):
    with open(symbols_file, "r") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            parts = line.split(":", 2)
            if len(parts) >= 2:
                try:
                    sid = int(parts[0])
                    sym = parts[1]
                    if sym not in symbol_to_id:
                        symbol_to_id[sym] = sid
                        max_id = max(max_id, sid)
                except ValueError:
                    pass
    print(f"Loaded {len(symbol_to_id)} existing symbols from {symbols_file} (max_id={max_id})")

print(f"Loading {ins_file}...")

try:
    with open(ins_file, 'r', encoding='utf-8') as f:
        data = json.load(f)
except Exception as e:
    print(f"Error loading JSON: {e}")
    sys.exit(1)

print(f"Total entries: {len(data)}")

# (symbol, multiplier); 去重时同 symbol 保留首次出现的乘数
contract_pairs = {}
skipped_expired = 0
skipped_type = 0

for key, val in data.items():
    # 1. 过滤过期
    if val.get('expired', True):
        skipped_expired += 1
        continue

    # 2. 过滤类型 (只保留期货 FUTURE)
    cls = val.get('class', '')
    if cls != 'FUTURE':
        skipped_type += 1
        continue

    # 3. 提取 CTP InstrumentID
    raw_id = val.get('instrument_id', '')
    if '.' in raw_id:
        ctp_id = raw_id.split('.')[-1]
    else:
        ctp_id = raw_id

    if not any(c.isdigit() for c in ctp_id):
        continue

    multiplier = val.get('volume_multiple', 1)
    if ctp_id not in contract_pairs:
        contract_pairs[ctp_id] = multiplier

# 按合约代码排序，保证 symbols.txt 与 config.yaml 中列表顺序一致且可复现
contracts = sorted(contract_pairs.keys())

print(f"Selected {len(contracts)} contracts (Skipped: {skipped_expired} expired, {skipped_type} non-future)")

if len(contracts) == 0:
    print("No contracts found! Check filters.")
    sys.exit(1)

# --- Step 1: Write symbols.txt (id:symbol:multiplier)，沿用已有 id，新合约从 max_id+1 起顺序分配，按 id 数字排序输出 ---
next_new_id = max_id + 1
rows = []
for c in contracts:
    if c in symbol_to_id:
        sid = symbol_to_id[c]
    else:
        sid = next_new_id
        symbol_to_id[c] = sid
        next_new_id += 1
    rows.append((sid, c, contract_pairs[c]))
rows.sort(key=lambda x: x[0])
with open(symbols_file, "w") as f:
    for sid, c, mult in rows:
        f.write(f"{sid}:{c}:{mult}\n")
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
