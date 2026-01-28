#pragma once

#include <cstdint>

// 全字段行情记录，支持深度回测与因子计算
struct TickRecord {
    // 基础信息
    char symbol[32];
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
