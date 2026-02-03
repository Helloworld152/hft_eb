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

struct AccountDetail {
    char broker_id[11];
    char account_id[13];
    double balance;         // 昨结 + 入金 - 出金
    double available;       // 可用资金
    double margin;          // 占用保证金
    double close_pnl;       // 平仓盈亏
    double position_pnl;    // 持仓盈亏
};

struct OrderReq {
    char account_id[16];
    char symbol[32];
    uint64_t symbol_id;
    char direction;   // 'B'uy or 'S'ell
    char offset_flag; // 'O'pen, 'C'lose, 'T'oday (上期所平今)
    double price;
    int volume;
};

struct CancelReq {
    char account_id[16];
    char symbol[32];
    char order_ref[13];
};

// 报单回报
struct OrderRtn {
    char account_id[16];
    char order_ref[13];
    char symbol[32];
    uint64_t symbol_id;
    char direction;      // 'B'/'S'
    char offset_flag;    // 'O'/'C'/'T'
    double limit_price;
    int volume_total;    // 报单总量
    int volume_traded;   // 已成交量
    char status;         // '0':全部成交, '1':部分成交, '3':未成交, '5':已退单
    char status_msg[81];
};

// 成交回报
struct TradeRtn {
    char account_id[16];
    char symbol[32];
    uint64_t symbol_id;
    char direction;      // 'B'/'S'
    char offset_flag;    // 'O'/'C'/'T'
    double price;
    int volume;
    char trade_id[21];
    char order_ref[13];
};

// 持仓明细
struct PositionDetail {
    char account_id[16];
    char symbol[32];
    uint64_t symbol_id;
    
    // 多头
    int long_td;
    int long_yd;
    double long_avg_price;
    
    // 假头
    int short_td;
    int short_yd;
    double short_avg_price;
    
    double net_pnl;
};

// 因子信号数据结构
struct SignalRecord {
    char source_id[32];    // 来源节点 ID (如 "FACTOR_RSI")
    char symbol[32];
    char factor_name[32];  // 因子/信号名称
    double value;          // 信号值
    uint64_t timestamp;    // 产生时间
};

struct ConnectionStatus {
    char account_id[16];
    char source[16]; // "CTP_TD", "CTP_MD" etc.
    char status;     // '0': Disconnected, '1': Connected, '2': Authenticated, '3': LoggedIn
    char msg[64];
};