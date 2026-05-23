---
name: hft_eb_dev
description: hft_eb 仓库开发——导航、修改、验证
---

# hft_eb 开发

## 快速定位

```
grep -rn "关键词" modules/ core/ include/ infra/  # 找代码
grep -rn "关键词" docs/                              # 找设计依据
```

目录分层：`core/` 协议+状态 | `infra/` 通用组件 | `modules/` 插件 | `hft_backtest/` Python 回测 | `conf/` YAML 配置

## 构建

```bash
# 最小构建（改单个模块后）
cd build && cmake .. && make <target>
# 如: make mod_sim_trade

# 完整构建+安装（改 Python/C++ 桥接后）
python3 setup.py bdist_wheel && pip install --force-reinstall hft_backtest/dist/hft_backtest-*.whl
```

## 验证

- 改 C++ 模块 → 最小构建目标
- 改 py_strategy/recorder/sim_trade → 重编 wheel + `python3 hft_backtest/test1.py`
- 改 protocol.h → 检查所有消费该事件/结构的模块
- 改 infra 组件 → 检查 `simple_matching_engine.h` / `tick_matching_engine.h` 的调用方

## 文档维护

大量代码修改后 → 主动问用户"是否更新对应文档"。
文档更新要求：只改受影响的部分，精炼，不堆砌过程。

## 规则

- 最小修改，不改无关文件
- `#pragma once`，不用 `#ifndef`
- 类名 PascalCase，成员变量 `snake_case_`，文件 `snake_case`
- `IntrusivePool` 要求 trivially destructible → 用 `char[N]`，别用 `std::string`
- 事件流：ORDER_REQ → Risk → ORDER_SEND → SimTrade → RTN_ORDER/RTN_TRADE
- CANCEL_REQ → Risk → CANCEL_SEND → SimTrade
- Python 策略只暴露 `config`，`_send_order`/`_cancel_order` 由 C++ 构造后属性注入

## 新增模块 checklist

```
[ ] modules/<name>/<name>_module.cpp   — 实现 IModule，末尾 EXPORT_MODULE
[ ] CMakeLists.txt                      — add_library + target_include_directories + target_link_libraries
[ ] 订阅事件 (init 里 bus_->subscribe)
[ ] config 参数用 config.count() 检查，给默认值
[ ] setup.py 和 engine.py 如需构建/加载此模块，同步更新
```

## 新增事件 checklist

```
[ ] engine/include/framework.h — EventType enum 加一项（插在 MAX_EVENTS 前）
[ ] 所有发布方 — bus_->publish(EVENT_XXX, ...)
[ ] 所有订阅方 — bus_->subscribe(EVENT_XXX, ...)
[ ] 是否需 Risk 转发（交易类事件 = 需要）
```

## 依赖规则

```
infra/  ← 不依赖任何业务层（framework/protocol 都不行）
core/   ← 依赖 infra/
modules/ ← 依赖 framework + core + infra
hft_backtest/ ← 依赖 modules/ 编译出的 .so
```

infra 组件必须 header-only，不依赖 `framework.h`、`protocol.h`。

## 回测开发

详见 [references/hft_backtest.md](references/hft_backtest.md)

## 危险操作

- 不要跑 `./run.sh`（会 kill 进程）
- 不要 `rm -rf build/` 再全量重编（慢）
- 改 `protocol.h` 字段顺序/大小 → 影响所有模块 ABI
