#include "../include/core_state.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace core {
namespace {

CoreServices g_core_services;

}  // namespace

bool PositionStore::is_shfe_ine(const char* exchange_id) {
    return exchange_id &&
           (std::strcmp(exchange_id, "SHFE") == 0 || std::strcmp(exchange_id, "INE") == 0);
}

bool PositionStore::get_position(const char* account_id, uint64_t symbol_id, PositionDetail* out) const {
    if (!account_id || account_id[0] == '\0' || symbol_id == 0 || !out) return false;
    auto it_acc = positions_.find(account_id);
    if (it_acc == positions_.end()) return false;
    auto it_pos = it_acc->second.find(symbol_id);
    if (it_pos == it_acc->second.end()) return false;
    *out = it_pos->second;
    return true;
}

void PositionStore::snapshot(std::vector<PositionDetail>* out) const {
    if (!out) return;
    out->clear();
    for (const auto& account_entry : positions_) {
        for (const auto& pos_entry : account_entry.second) {
            out->push_back(pos_entry.second);
        }
    }
}

void PositionStore::snapshot_account(const char* account_id, std::vector<PositionDetail>* out) const {
    if (!out) return;
    out->clear();
    auto it_acc = positions_.find(account_id ? account_id : "");
    if (it_acc == positions_.end()) return;
    for (const auto& pos_entry : it_acc->second) {
        out->push_back(pos_entry.second);
    }
}

void PositionStore::apply_trade(const TradeRtn& rtn) {
    std::string account_id = rtn.account_id[0] != '\0' ? rtn.account_id : "default";
    uint64_t symbol_id = rtn.symbol_id;
    if (symbol_id == 0) return;

    auto& pos = positions_[account_id][symbol_id];
    if (pos.symbol_id == 0) {
        pos.symbol_id = symbol_id;
        std::strncpy(pos.symbol, rtn.symbol, sizeof(pos.symbol) - 1);
        std::strncpy(pos.account_id, account_id.c_str(), sizeof(pos.account_id) - 1);
        std::strncpy(pos.exchange_id, rtn.exchange_id, sizeof(pos.exchange_id) - 1);
    }

    const bool is_shfe = is_shfe_ine(rtn.exchange_id);
    if (rtn.offset_flag == 'O') {
        if (rtn.direction == 'B') pos.long_td += rtn.volume;
        else pos.short_td += rtn.volume;
    } else if (rtn.direction == 'S') {
        if (is_shfe) {
            if (rtn.offset_flag == 'T') pos.long_td -= rtn.volume;
            else pos.long_yd -= rtn.volume;
        } else if (pos.long_yd >= rtn.volume) {
            pos.long_yd -= rtn.volume;
        } else {
            const int remain = rtn.volume - pos.long_yd;
            pos.long_yd = 0;
            pos.long_td -= remain;
        }
    } else {
        if (is_shfe) {
            if (rtn.offset_flag == 'T') pos.short_td -= rtn.volume;
            else pos.short_yd -= rtn.volume;
        } else if (pos.short_yd >= rtn.volume) {
            pos.short_yd -= rtn.volume;
        } else {
            const int remain = rtn.volume - pos.short_yd;
            pos.short_yd = 0;
            pos.short_td -= remain;
        }
    }

    pos.long_td = std::max(0, pos.long_td);
    pos.long_yd = std::max(0, pos.long_yd);
    pos.short_td = std::max(0, pos.short_td);
    pos.short_yd = std::max(0, pos.short_yd);
}

void PositionStore::apply_rsp_pos(const PositionDetail& p) {
    if (p.account_id[0] == '\0' || p.symbol_id == 0) return;

    auto& local = positions_[p.account_id][p.symbol_id];
    if (local.symbol_id == 0) {
        std::strncpy(local.symbol, p.symbol, sizeof(local.symbol) - 1);
        std::strncpy(local.account_id, p.account_id, sizeof(local.account_id) - 1);
        std::strncpy(local.exchange_id, p.exchange_id, sizeof(local.exchange_id) - 1);
        local.symbol_id = p.symbol_id;
    }

    const bool is_shfe = is_shfe_ine(p.exchange_id);
    if (p.direction == '2') {
        if (is_shfe) {
            if (p.position_date == '1') local.long_td = p.long_td;
            else if (p.position_date == '2') local.long_yd = p.long_yd;
        } else {
            local.long_td = p.long_td;
            local.long_yd = p.long_yd;
        }
        local.long_avg_price = p.long_avg_price;
        local.long_pnl = p.net_pnl;
    } else if (p.direction == '3') {
        if (is_shfe) {
            if (p.position_date == '1') local.short_td = p.short_td;
            else if (p.position_date == '2') local.short_yd = p.short_yd;
        } else {
            local.short_td = p.short_td;
            local.short_yd = p.short_yd;
        }
        local.short_avg_price = p.short_avg_price;
        local.short_pnl = p.net_pnl;
    }
    local.net_pnl = local.long_pnl + local.short_pnl;
}

void PositionStore::apply_reset(const CacheReset& reset) {
    if ((reset.reset_type & 0x1u) == 0u && reset.reset_type != 0xFFu) return;
    if (reset.account_id[0] == '\0') positions_.clear();
    else positions_.erase(reset.account_id);
}

uint64_t OrderStore::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool OrderStore::get_order(const char* account_id, const char* order_ref, OrderState* out) const {
    if (!account_id || !order_ref || order_ref[0] == '\0' || !out) return false;
    auto it_acc = orders_.find(account_id);
    if (it_acc == orders_.end()) return false;
    auto it_order = it_acc->second.find(order_ref);
    if (it_order == it_acc->second.end()) return false;
    *out = it_order->second;
    return true;
}

bool OrderStore::get_order_by_sys_id(const char* account_id,
                                     const char* order_sys_id,
                                     OrderState* out) const {
    if (!account_id || !order_sys_id || order_sys_id[0] == '\0' || !out) return false;
    auto it_acc = sys_orders_.find(account_id);
    if (it_acc == sys_orders_.end()) return false;
    auto it_ref = it_acc->second.find(order_sys_id);
    if (it_ref == it_acc->second.end()) return false;
    return get_order(account_id, it_ref->second.c_str(), out);
}

void OrderStore::snapshot(std::vector<OrderState>* out) const {
    if (!out) return;
    out->clear();
    for (const auto& account_entry : orders_) {
        for (const auto& order_entry : account_entry.second) {
            out->push_back(order_entry.second);
        }
    }
}

void OrderStore::snapshot_account(const char* account_id, std::vector<OrderState>* out) const {
    if (!out) return;
    out->clear();
    auto it_acc = orders_.find(account_id ? account_id : "");
    if (it_acc == orders_.end()) return;
    for (const auto& order_entry : it_acc->second) {
        out->push_back(order_entry.second);
    }
}

void OrderStore::apply_order_rtn(const OrderRtn& order) {
    std::string account_id = order.account_id[0] != '\0' ? order.account_id : "default";
    std::string order_ref = order.order_ref[0] != '\0' ? order.order_ref : std::to_string(order.client_id);

    auto& state = orders_[account_id][order_ref];
    if (state.order_ref[0] == '\0') {
        std::strncpy(state.account_id, account_id.c_str(), sizeof(state.account_id) - 1);
        std::strncpy(state.order_ref, order_ref.c_str(), sizeof(state.order_ref) - 1);
    }
    std::strncpy(state.order_sys_id, order.order_sys_id, sizeof(state.order_sys_id) - 1);
    std::strncpy(state.exchange_id, order.exchange_id, sizeof(state.exchange_id) - 1);
    std::strncpy(state.symbol, order.symbol, sizeof(state.symbol) - 1);
    state.symbol_id = order.symbol_id;
    state.direction = order.direction;
    state.offset_flag = order.offset_flag;
    state.limit_price = order.limit_price;
    state.volume_total = order.volume_total;
    state.volume_traded = order.volume_traded;
    state.volume_canceled = std::max(0, order.volume_total - order.volume_traded);
    state.status = order.status;
    std::strncpy(state.status_msg, order.status_msg, sizeof(state.status_msg) - 1);
    state.update_ts = now_ms();

    if (order.order_sys_id[0] != '\0') {
        sys_orders_[account_id][order.order_sys_id] = order_ref;
    }
}

void OrderStore::apply_trade_rtn(const TradeRtn& trade) {
    std::string account_id = trade.account_id[0] != '\0' ? trade.account_id : "default";
    std::string order_ref;

    if (trade.order_sys_id[0] != '\0') {
        auto it_acc = sys_orders_.find(account_id);
        if (it_acc != sys_orders_.end()) {
            auto it_ref = it_acc->second.find(trade.order_sys_id);
            if (it_ref != it_acc->second.end()) {
                order_ref = it_ref->second;
            }
        }
    }
    if (order_ref.empty() && trade.order_ref[0] != '\0') {
        order_ref = trade.order_ref;
    }
    if (order_ref.empty()) return;

    auto& state = orders_[account_id][order_ref];
    if (state.order_ref[0] == '\0') {
        std::strncpy(state.account_id, account_id.c_str(), sizeof(state.account_id) - 1);
        std::strncpy(state.order_ref, order_ref.c_str(), sizeof(state.order_ref) - 1);
        std::strncpy(state.symbol, trade.symbol, sizeof(state.symbol) - 1);
        state.symbol_id = trade.symbol_id;
        state.direction = trade.direction;
        state.offset_flag = trade.offset_flag;
    }
    if (trade.order_sys_id[0] != '\0') {
        std::strncpy(state.order_sys_id, trade.order_sys_id, sizeof(state.order_sys_id) - 1);
        sys_orders_[account_id][trade.order_sys_id] = order_ref;
    }
    state.volume_traded += trade.volume;
    if (state.volume_total > 0 && state.volume_traded >= state.volume_total) {
        state.status = '0';
        state.volume_traded = state.volume_total;
    } else if (state.volume_traded > 0) {
        state.status = '1';
    }
    state.volume_canceled = std::max(0, state.volume_total - state.volume_traded);
    state.update_ts = now_ms();
}

void OrderStore::apply_reset(const CacheReset& reset) {
    if ((reset.reset_type & 0x2u) == 0u && reset.reset_type != 0xFFu) return;
    if (reset.account_id[0] == '\0') {
        orders_.clear();
        sys_orders_.clear();
    } else {
        orders_.erase(reset.account_id);
        sys_orders_.erase(reset.account_id);
    }
}

bool AccountStore::get_account(const char* account_id, AccountDetail* out) const {
    if (!account_id || account_id[0] == '\0' || !out) return false;
    auto it = accounts_.find(account_id);
    if (it == accounts_.end()) return false;
    *out = it->second;
    return true;
}

void AccountStore::snapshot(std::vector<AccountDetail>* out) const {
    if (!out) return;
    out->clear();
    for (const auto& account_entry : accounts_) {
        out->push_back(account_entry.second);
    }
}

void AccountStore::apply_account(const AccountDetail& account) {
    if (account.account_id[0] == '\0') return;
    accounts_[account.account_id] = account;
}

void AccountStore::apply_reset(const CacheReset& reset) {
    if ((reset.reset_type & 0x4u) == 0u && reset.reset_type != 0xFFu) return;
    if (reset.account_id[0] == '\0') accounts_.clear();
    else accounts_.erase(reset.account_id);
}

PositionService::PositionService(size_t queue_capacity) {
    (void)queue_capacity;
}

bool PositionService::enqueue_trade(const TradeRtn& trade) {
    store_.apply_trade(trade);
    return true;
}

bool PositionService::enqueue_rsp_pos(const PositionDetail& pos) {
    store_.apply_rsp_pos(pos);
    return true;
}

bool PositionService::enqueue_reset(const CacheReset& reset) {
    store_.apply_reset(reset);
    return true;
}

void PositionService::start() {}
void PositionService::stop() {}

OrderService::OrderService(size_t queue_capacity) {
    (void)queue_capacity;
}

bool OrderService::enqueue_order_rtn(const OrderRtn& order) {
    store_.apply_order_rtn(order);
    return true;
}

bool OrderService::enqueue_trade_rtn(const TradeRtn& trade) {
    store_.apply_trade_rtn(trade);
    return true;
}

bool OrderService::enqueue_reset(const CacheReset& reset) {
    store_.apply_reset(reset);
    return true;
}

void OrderService::start() {}
void OrderService::stop() {}

AccountService::AccountService(size_t queue_capacity) {
    (void)queue_capacity;
}

bool AccountService::enqueue_account(const AccountDetail& account) {
    store_.apply_account(account);
    return true;
}

bool AccountService::enqueue_reset(const CacheReset& reset) {
    store_.apply_reset(reset);
    return true;
}

void AccountService::start() {}
void AccountService::stop() {}

void CoreServicesRegistry::set(const CoreServices& services) {
    g_core_services = services;
}

const CoreServices& CoreServicesRegistry::get() {
    return g_core_services;
}

void CoreServicesRegistry::clear() {
    set(CoreServices{});
}

}  // namespace core
