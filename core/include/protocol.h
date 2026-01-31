#pragma once

#include <cstdint>

// 全字段行情记录，支持深度回测与因子计算
// 对齐到 64 字节（缓存行），提升缓存性能
struct alignas(64) TickRecord {
    // 基础信息
    char symbol[32];
    uint64_t symbol_id;    // Mapped from conf/symbols.txt
    uint32_t trading_day; // YYYYMMDD
    uint64_t update_time; // HHMMSSmmm

    // 价格与成交
    double last_price;
    int volume;
    double turnover;      // 成交额
    double open_interest; // 持仓量
    
    // 统计数据
    double upper_limit;   // 涨停价
    double lower_limit;   // 跌停价
    double open_price;
    double highest_price;
    double lowest_price;
    double pre_close_price;

    // 五档行情
    double bid_price[5];
    int bid_volume[5];
    double ask_price[5];
    int ask_volume[5];
};

enum KlineInterval {
    K_1M = 1,
    K_5M = 5,
    K_15M = 15,
    K_1H = 60,
    K_1D = 1440
};

struct KlineRecord {
    char symbol[32];
    uint64_t symbol_id;    // Mapped ID
    uint32_t trading_day; // 交易日 YYYYMMDD
    uint64_t start_time;  // 周期起始时间 HHMMSSmmm
    double open;
    double high;
    double low;
    double close;
    int volume;           // 周期内成交量增量
    double turnover;      // 周期内成交额增量
    double open_interest; // 周期末持仓量
    KlineInterval interval;
};
