# 因子计算插件设计 (Factor Plugin)

## 1. 核心目标
将复杂的量化指标（如：多因子模型、机器学习预测值）从策略逻辑中解耦，实现“因子库”的动态化。

## 2. 接口定义 (`IFactorNode`)
因子插件同样遵循二级插件架构。

```cpp
class IFactor {
public:
    virtual ~IFactor() = default;
    virtual void init(const ConfigMap& config) = 0;
    virtual double update(const TickRecord* tick) = 0;
};
```

## 3. 插件链 (Factor Pipeline)
策略树中的 Leaf 节点可以挂载多个因子插件：
1. **Factor A (MA)** -> 注入 LeafNode。
2. **Factor B (Volatility)** -> 注入 LeafNode。
3. **LeafNode Logic**: `if (factor_a > factor_b) send_order();`

## 4. 优势
- **复用性**: 同一个“移动平均因子”可以被多个策略重用。
- **热更新**: 修改因子计算公式只需重新编译该插件的 `.so`，无需触动核心交易逻辑。
