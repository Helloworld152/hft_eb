# 构建与验证

## C++ 模块构建

```bash
cd /home/rying/hft_eb/build
cmake .. && make <target> -j$(nproc)
```

常用 target：`mod_sim_trade` `mod_lob_sim_trade` `mod_py_strategy` `mod_risk` `mod_replay` `mod_backtest_recorder` `hft_core` `hft_engine_lib` `_core`

## Python 回测构建

```bash
cd /home/rying/hft_eb
python3 setup.py bdist_wheel
pip install --force-reinstall hft_backtest/dist/hft_backtest-*.whl
rm -rf hft_backtest/results/au2606_test
python3 hft_backtest/test1.py
```

改以下任意模块后必须重编 wheel：`py_strategy_module.cpp` `sim_trade_module.cpp` `risk_module.cpp` `backtest_recorder_module.cpp`

## 验证清单

| 改动范围 | 验证方式 |
|----------|----------|
| sim_trade 撮合逻辑 | `make mod_sim_trade` → wheel → test1.py，检查 orders.csv 状态流转 |
| py_strategy 下单/撤单 | wheel → test1.py，检查终端 `[下单]` `[回报]` `[成交]` |
| risk 转发 | `make mod_risk` → wheel → test1.py |
| recorder CSV | `make mod_backtest_recorder` → wheel → test1.py，检查 CSV header 和数据对齐 |
| infra 组件 | `make` 受影响 target，不改 API 则无需 wheel |
| protocol.h 字段 | 检查所有 `grep -rn "字段名" modules/` 的引用 |

## 不要做的事

- 改 `BaseStrategy.__init__` 签名 → 所有策略继承它
- 改 Recorder CSV header 不同步数据行 → 列数对不上
- 往 Python `__init__` 传内部回调 → 用 `PyObject_SetAttrString` 注入
