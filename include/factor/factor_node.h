#pragma once

#include <string>
#include <vector>
#include "../framework.h"
#include "../../core/include/protocol.h"

// 计算上下文：由 FactorDAGModule 注入
struct FactorContext {
    const TickRecord* tick = nullptr;
    const KlineRecord* kline = nullptr;
};

// 因子节点接口：仅负责局部计算
class IFactorNode {
public:
    virtual ~IFactorNode() = default;
    virtual void init(const ConfigMap& config) = 0;
    virtual double compute(const FactorContext& ctx, const std::vector<double>& inputs) = 0;
};

// 每个因子 .so 必须实现
typedef IFactorNode* (*CreateFactorFunc)();
#define EXPORT_FACTOR(CLASS_NAME) \
    extern "C" { \
        IFactorNode* create_factor() { return new CLASS_NAME(); } \
    }
