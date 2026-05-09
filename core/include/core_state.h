#pragma once

#include <memory>
#include <string>
#include <vector>
#include "hash_containers.h"

#include "protocol.h"

namespace core {

class PositionStore {
public:
    bool get_position(const char* account_id, uint64_t symbol_id, PositionDetail* out) const;
    void snapshot(std::vector<PositionDetail>* out) const;
    void snapshot_account(const char* account_id, std::vector<PositionDetail>* out) const;

    void apply_trade(const TradeRtn& trade);
    void apply_rsp_pos(const PositionDetail& pos);
    void apply_reset(const CacheReset& reset);

private:
    using AccountMap = FastHashMap<uint64_t, PositionDetail>;
    using RootMap = FastHashMap<std::string, AccountMap>;

    static bool is_shfe_ine(const char* exchange_id);

    RootMap positions_;
};

class OrderStore {
public:
    bool get_order(const char* account_id, const char* order_ref, OrderState* out) const;
    bool get_order_by_sys_id(const char* account_id, const char* order_sys_id, OrderState* out) const;
    void snapshot(std::vector<OrderState>* out) const;
    void snapshot_account(const char* account_id, std::vector<OrderState>* out) const;

    void apply_order_rtn(const OrderRtn& order);
    void apply_trade_rtn(const TradeRtn& trade);
    void apply_reset(const CacheReset& reset);

private:
    using RefMap = FastHashMap<std::string, OrderState>;
    using RootMap = FastHashMap<std::string, RefMap>;
    using SysIndexMap = FastHashMap<std::string, FastHashMap<std::string, std::string>>;

    static uint64_t now_ms();

    RootMap orders_;
    SysIndexMap sys_orders_;
};

class AccountStore {
public:
    bool get_account(const char* account_id, AccountDetail* out) const;
    void snapshot(std::vector<AccountDetail>* out) const;

    void apply_account(const AccountDetail& account);
    void apply_reset(const CacheReset& reset);

private:
    FastHashMap<std::string, AccountDetail> accounts_;
};

class PositionService {
public:
    explicit PositionService(size_t queue_capacity = 0);

    bool enqueue_trade(const TradeRtn& trade);
    bool enqueue_rsp_pos(const PositionDetail& pos);
    bool enqueue_reset(const CacheReset& reset);

    void start();
    void stop();

    const PositionStore& store() const noexcept { return store_; }

private:
    PositionStore store_;
};

class OrderService {
public:
    explicit OrderService(size_t queue_capacity = 0);

    bool enqueue_order_rtn(const OrderRtn& order);
    bool enqueue_trade_rtn(const TradeRtn& trade);
    bool enqueue_reset(const CacheReset& reset);

    void start();
    void stop();

    const OrderStore& store() const noexcept { return store_; }

private:
    OrderStore store_;
};

class AccountService {
public:
    explicit AccountService(size_t queue_capacity = 0);

    bool enqueue_account(const AccountDetail& account);
    bool enqueue_reset(const CacheReset& reset);

    void start();
    void stop();

    const AccountStore& store() const noexcept { return store_; }

private:
    AccountStore store_;
};

struct CoreServices {
    PositionService* position_service = nullptr;
    const PositionStore* position_store = nullptr;
    OrderService* order_service = nullptr;
    const OrderStore* order_store = nullptr;
    AccountService* account_service = nullptr;
    const AccountStore* account_store = nullptr;
};

class CoreServicesRegistry {
public:
    static void set(const CoreServices& services);
    static const CoreServices& get();
    static void clear();
};

}  // namespace core
