#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>

inline uint64_t local_timestamp_yyyymmddhhmmssmmm() {
    using namespace std::chrono;

    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t now_time = system_clock::to_time_t(now);

    std::tm local_tm{};
    localtime_r(&now_time, &local_tm);

    uint64_t ts = static_cast<uint64_t>(local_tm.tm_year + 1900);
    ts = ts * 100 + static_cast<uint64_t>(local_tm.tm_mon + 1);
    ts = ts * 100 + static_cast<uint64_t>(local_tm.tm_mday);
    ts = ts * 100 + static_cast<uint64_t>(local_tm.tm_hour);
    ts = ts * 100 + static_cast<uint64_t>(local_tm.tm_min);
    ts = ts * 100 + static_cast<uint64_t>(local_tm.tm_sec);
    ts = ts * 1000 + static_cast<uint64_t>(ms.count());
    return ts;
}

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
    double frozen_cash;     // 冻结资金
    double frozen_margin;   // 冻结保证金
    double frozen_commission; // 冻结手续费
    double close_pnl;       // 平仓盈亏
    double position_pnl;    // 持仓盈亏
};

struct OrderReq {
    uint64_t client_id;  // 内部唯一订单ID
    char order_ref[13];  // 映射ID (由 OrderManager 生成)
    char account_id[16];
    char symbol[32];
    uint64_t symbol_id;
    char direction;   // 'B'uy or 'S'ell
    char offset_flag; // 'O'pen, 'C'lose, 'T'oday (上期所平今)
    double price;
    int volume;
};

struct CancelReq {
    uint64_t client_id;  // 指向原报单的内部唯一ID
    char account_id[16];
    char symbol[32];
    char order_ref[13];
    char order_sys_id[21]; // 交易所系统 ID
};

// 报单回报
struct OrderRtn {
    uint64_t client_id;  // 对应请求的内部ID
    char account_id[16];
    char order_ref[13];
    char order_sys_id[21]; // 交易所系统 ID
    char exchange_id[9];   // 交易所代码
    char symbol[32];
    uint64_t symbol_id;
    char direction;      // 'B'/'S'
    char offset_flag;    // 'O'/'C'/'T'
    double limit_price;
    int volume_total;    // 报单总量
    int volume_traded;   // 已成交量
    uint64_t insert_time; // 本地插入时间 YYYYMMDDHHMMSSmmm
    uint64_t update_time; // 本地更新时间 YYYYMMDDHHMMSSmmm
    char status;         // '0':全部成交, '1':部分成交, '3':未成交, '5':已退单
    char status_msg[81];
};

// 成交回报
struct TradeRtn {
    uint64_t client_id;  // 对应请求的内部ID
    char account_id[16];
    char exchange_id[9];   // 交易所代码
    char symbol[32];
    uint64_t symbol_id;
    char direction;      // 'B'/'S'
    char offset_flag;    // 'O'/'C'/'T'
    double price;
    int volume;
    uint64_t trade_time; // 本地成交时间 YYYYMMDDHHMMSSmmm
    char trade_id[21];
    char liquidity_role; // 'M' = maker, 'T' = taker
    char order_ref[13];
    char order_sys_id[21]; // 交易所系统 ID
};

// 持仓明细
struct PositionDetail {
    char account_id[16];
    char symbol[32];
    char exchange_id[9]; // 交易所代码
    uint64_t symbol_id;
    char direction;      // '1':Net, '2':Long, '3':Short
    char position_date;  // '1':Today, '2':History, '3':Both/Total
    
    // 多头
    int long_td;
    int long_yd;
    double long_avg_price;
    double long_pnl;
    
    // 空头
    int short_td;
    int short_yd;
    double short_avg_price;
    double short_pnl;
    
    double net_pnl;
};

struct OrderState {
    char account_id[16];
    char order_ref[13];
    char order_sys_id[21];
    char exchange_id[9];
    char symbol[32];
    uint64_t symbol_id;
    char direction;
    char offset_flag;
    double limit_price;
    int volume_total;
    int volume_traded;
    int volume_canceled;
    char status;
    char status_msg[81];
    uint64_t update_ts;
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

struct CacheReset {
    char account_id[16];
    uint32_t trading_day; // YYYYMMDD
    uint32_t reset_type;  // 0x1: Position, 0x2: Orders, 0xFF: All
    char reason[64];
};

struct PositionUpdate {
    enum class Type : uint8_t { Trade, RspPos, Reset };
    Type type;
    TradeRtn trade;
    PositionDetail pos;
    CacheReset reset;
};

struct OrderUpdate {
    enum class Type : uint8_t { OrderRtn, TradeRtn, Reset };
    Type type;
    OrderRtn order;
    TradeRtn trade;
    CacheReset reset;
};

struct AccountUpdate {
    enum class Type : uint8_t { Account, Reset };
    Type type;
    AccountDetail account;
    CacheReset reset;
};
