#pragma once

#include "intrusive_pool.h"

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <vector>

class TickMatchingEngine {
public:
    using index_type = uint32_t;
    static constexpr index_type npos = UINT32_MAX;

    enum Side : uint8_t { BUY = 1, SELL = 2 };

    struct OrderNode {
        uint64_t order_id = 0;
        double price = 0.0;
        int volume = 0;
        int volume_total = 0;
        int volume_traded = 0;
        Side side = BUY;
        index_type prev = npos;
        index_type next = npos;
    };

    struct Trade {
        uint64_t taker_id = 0;  // 0 = 外部 tick
        uint64_t maker_id = 0;
        double price = 0.0;
        int qty = 0;
    };

    struct Output {
        std::vector<Trade> trades;
        bool resting = false;
    };

    explicit TickMatchingEngine(size_t max_orders)
        : pool_(max_orders), order_index_(max_orders + 1, npos) {}

    bool submit(uint64_t order_id, Side side, double price, int qty,
                bool is_market, Output& out) {
        out.trades.clear();
        out.resting = false;

        if (qty <= 0 || order_id == 0 || order_id >= order_index_.size()
            || order_index_[order_id] != npos)
            return false;

        int remaining = qty;
        match_internal(order_id, side, price, remaining, out);

        if (remaining == 0) return true;
        if (is_market) return false;

        index_type idx = pool_.allocate();
        if (idx == npos) return false;

        pool_[idx] = {order_id, price, remaining, qty, qty - remaining, side, npos, npos};
        order_index_[order_id] = idx;

        if (side == BUY) push_back(buy_head_, buy_tail_, buy_size_, idx);
        else             push_back(sell_head_, sell_tail_, sell_size_, idx);
        out.resting = true;
        return true;
    }

    void apply_tick(const double* bid_prices, const int* bid_vols, int bid_depth,
                    const double* ask_prices, const int* ask_vols, int ask_depth,
                    Output& out) {
        out.trades.clear();
        out.resting = false;

        index_type cur = buy_head_;
        while (cur != npos) {
            OrderNode& buy = pool_[cur];
            index_type next = buy.next;
            int consumed = try_match_depth(cur, ask_prices, ask_vols, ask_depth, out);
            if (consumed == 0) { cur = next; continue; }
            buy.volume -= consumed;
            buy.volume_traded += consumed;
            if (buy.volume == 0) {
                order_index_[buy.order_id] = npos;
                remove(buy_head_, buy_tail_, buy_size_, cur);
                pool_.release_unchecked(cur);
            }
            cur = next;
        }

        cur = sell_head_;
        while (cur != npos) {
            OrderNode& sell = pool_[cur];
            index_type next = sell.next;
            int consumed = try_match_depth(cur, bid_prices, bid_vols, bid_depth, out);
            if (consumed == 0) { cur = next; continue; }
            sell.volume -= consumed;
            sell.volume_traded += consumed;
            if (sell.volume == 0) {
                order_index_[sell.order_id] = npos;
                remove(sell_head_, sell_tail_, sell_size_, cur);
                pool_.release_unchecked(cur);
            }
            cur = next;
        }
    }

    bool cancel(uint64_t order_id, Output& out) {
        out.trades.clear();
        out.resting = false;
        if (order_id == 0 || order_id >= order_index_.size()) return false;
        index_type idx = order_index_[order_id];
        if (idx == npos) return false;

        OrderNode& order = pool_[idx];
        if (order.side == BUY) remove(buy_head_, buy_tail_, buy_size_, idx);
        else                   remove(sell_head_, sell_tail_, sell_size_, idx);
        order_index_[order_id] = npos;
        pool_.release_unchecked(idx);
        return true;
    }

    size_t buy_count() const noexcept { return buy_size_; }
    size_t sell_count() const noexcept { return sell_size_; }

private:
    void match_internal(uint64_t taker_id, Side side, double limit_price,
                        int& qty, Output& out) {
        index_type& head = (side == BUY) ? sell_head_ : buy_head_;
        size_t& count = (side == BUY) ? sell_size_ : buy_size_;

        if (limit_price <= 0.0)
            limit_price = (side == BUY) ? DBL_MAX : -DBL_MAX;

        index_type cur = head;
        while (cur != npos && qty > 0) {
            OrderNode& resting = pool_[cur];
            index_type next = resting.next;

            bool crosses = (side == BUY) ? (resting.price <= limit_price)
                                          : (resting.price >= limit_price);
            if (!crosses) { cur = next; continue; }

            int match_qty = std::min(qty, resting.volume);
            out.trades.push_back({taker_id, resting.order_id, resting.price, match_qty});

            qty -= match_qty;
            resting.volume -= match_qty;
            resting.volume_traded += match_qty;

            if (resting.volume == 0) {
                order_index_[resting.order_id] = npos;
                remove(head, (side == BUY) ? sell_tail_ : buy_tail_, count, cur);
                pool_.release_unchecked(cur);
            }
            cur = next;
        }
    }

    int try_match_depth(index_type idx,
                        const double* prices, const int* vols, int depth,
                        Output& out) {
        OrderNode& order = pool_[idx];
        for (int i = 0; i < depth; ++i) {
            if (vols[i] <= 0 || prices[i] <= 0.0) continue;
            bool crosses = (order.side == BUY) ? (order.price >= prices[i])
                                                : (order.price <= prices[i]);
            if (!crosses) continue;
            int match_qty = std::min(order.volume, vols[i]);
            out.trades.push_back({0, order.order_id, prices[i], match_qty});
            return match_qty;
        }
        return 0;
    }

    void push_back(index_type& head, index_type& tail, size_t& count, index_type idx) {
        pool_[idx].prev = tail;
        pool_[idx].next = npos;
        if (tail != npos) pool_[tail].next = idx;
        else head = idx;
        tail = idx;
        ++count;
    }

    void remove(index_type& head, index_type& tail, size_t& count, index_type idx) {
        OrderNode& node = pool_[idx];
        if (node.prev != npos) pool_[node.prev].next = node.next;
        else head = node.next;
        if (node.next != npos) pool_[node.next].prev = node.prev;
        else tail = node.prev;
        --count;
    }

    IntrusivePool<OrderNode> pool_;
    std::vector<index_type> order_index_;

    index_type buy_head_ = npos, buy_tail_ = npos;
    index_type sell_head_ = npos, sell_tail_ = npos;
    size_t buy_size_ = 0, sell_size_ = 0;
};
