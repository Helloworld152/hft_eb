#pragma once

#include "intrusive_pool.h"

#include <cstdint>
#include <vector>

class SimpleMatchingEngine {
public:
    using index_type = IntrusivePool<uint32_t>::index_type;
    static constexpr index_type npos = IntrusivePool<uint32_t>::npos;

    enum Side : uint8_t { BUY = 1, SELL = 2 };
    enum EntrustType : uint8_t { ADD = 1, CANCEL = 2 };

    struct EntrustTick {
        uint64_t order_id;
        Side side;
        EntrustType type;
        int64_t price;
        int qty;
    };

    struct TradeTick {
        uint64_t bid_order_id;
        uint64_t ask_order_id;
        int64_t price;
        int qty;
    };

    struct Output {
        std::vector<EntrustTick> entrusts;
        std::vector<TradeTick> trades;

        void clear() {
            entrusts.clear();
            trades.clear();
        }
    };

    explicit SimpleMatchingEngine(size_t max_orders,
                                  size_t max_levels,
                                  int64_t price_base = 0,
                                  size_t price_range = 0)
        : orders_(max_orders),
          levels_(max_levels),
          order_index_(max_orders + 1, npos),
          price_base_(price_base),
          bid_level_index_(price_range, npos),
          ask_level_index_(price_range, npos) {}

    bool submit(uint64_t order_id, Side side, int64_t price, int qty, Output& out) {
        return submit_impl(order_id, side, price, qty, &out);
    }

    bool submit(uint64_t order_id, Side side, int64_t price, int qty) {
        return submit_impl(order_id, side, price, qty, nullptr);
    }

    bool cancel(uint64_t order_id, Output& out) {
        return cancel_impl(order_id, &out);
    }

    bool cancel(uint64_t order_id) {
        return cancel_impl(order_id, nullptr);
    }

    size_t live_orders() const noexcept {
        return orders_.size();
    }

    size_t live_levels() const noexcept {
        return levels_.size();
    }

private:
    struct OrderNode {
        uint64_t order_id;
        int64_t price;
        int qty;
        Side side;
        index_type level_idx;
        index_type prev;
        index_type next;
    };

    struct LevelNode {
        int64_t price;
        int total_qty;
        index_type head;
        index_type tail;
        index_type prev;
        index_type next;
    };

    bool submit_impl(uint64_t order_id, Side side, int64_t price, int qty, Output* out) {
        if (qty <= 0 || order_id >= order_index_.size() || order_index_[order_id] != npos) return false;

        int remaining = qty;
        if (side == BUY) {
            match_buy(order_id, price, remaining, out);
        } else {
            match_sell(order_id, price, remaining, out);
        }

        if (remaining == 0) return true;
        return add_resting_order(order_id, side, price, remaining, out);
    }

    bool cancel_impl(uint64_t order_id, Output* out) {
        if (order_id >= order_index_.size()) return false;

        const index_type order_idx = order_index_[order_id];
        if (order_idx == npos) return false;
        OrderNode& order = orders_[order_idx];
        if (out) out->entrusts.push_back({order.order_id, order.side, CANCEL, order.price, order.qty});

        remove_order(order_idx);
        order_index_[order_id] = npos;
        orders_.release_unchecked(order_idx);
        return true;
    }

    void match_buy(uint64_t order_id, int64_t limit_price, int& qty, Output* out) {
        while (qty > 0 && ask_head_ != npos) {
            if (levels_[ask_head_].price > limit_price) break;
            match_level(order_id, BUY, ask_head_, qty, out);
        }
    }

    void match_sell(uint64_t order_id, int64_t limit_price, int& qty, Output* out) {
        while (qty > 0 && bid_head_ != npos) {
            if (levels_[bid_head_].price < limit_price) break;
            match_level(order_id, SELL, bid_head_, qty, out);
        }
    }

    void match_level(uint64_t incoming_id, Side incoming_side, index_type level_idx, int& qty, Output* out) {
        LevelNode& level = levels_[level_idx];
        while (qty > 0 && level.head != npos) {
            const index_type resting_idx = level.head;
            OrderNode& resting = orders_[resting_idx];
            const int trade_qty = qty < resting.qty ? qty : resting.qty;

            if (out) {
                if (incoming_side == BUY) {
                    out->trades.push_back({incoming_id, resting.order_id, resting.price, trade_qty});
                } else {
                    out->trades.push_back({resting.order_id, incoming_id, resting.price, trade_qty});
                }
            }

            qty -= trade_qty;
            resting.qty -= trade_qty;
            level.total_qty -= trade_qty;

            if (resting.qty == 0) {
                order_index_[resting.order_id] = npos;
                remove_order(resting_idx);
                orders_.release_unchecked(resting_idx);
                if (!levels_.get(level_idx)) break;
            }
        }
    }

    bool add_resting_order(uint64_t order_id, Side side, int64_t price, int qty, Output* out) {
        index_type level_idx = get_or_add_level(side, price);
        if (level_idx == npos) return false;

        index_type order_idx = orders_.allocate();
        if (order_idx == npos) {
            cleanup_empty_level(side, level_idx);
            return false;
        }

        LevelNode& level = levels_[level_idx];
        orders_[order_idx] = {order_id, price, qty, side, level_idx, level.tail, npos};
        if (level.tail != npos) {
            orders_[level.tail].next = order_idx;
        } else {
            level.head = order_idx;
        }
        level.tail = order_idx;
        level.total_qty += qty;
        order_index_[order_id] = order_idx;
        if (out) out->entrusts.push_back({order_id, side, ADD, price, qty});
        return true;
    }

    index_type get_or_add_level(Side side, int64_t price) {
        if (side == BUY) {
            return get_or_add_bid_level(price);
        }
        return get_or_add_ask_level(price);
    }

    index_type get_or_add_bid_level(int64_t price) {
        index_type* indexed = price_index(bid_level_index_, price);
        if (indexed && *indexed != npos) return *indexed;

        index_type prev = npos;
        index_type cur = bid_head_;
        while (cur != npos && levels_[cur].price > price) {
            prev = cur;
            cur = levels_[cur].next;
        }
        if (cur != npos && levels_[cur].price == price) return cur;
        index_type idx = insert_level(BUY, price, prev, cur);
        if (indexed && idx != npos) *indexed = idx;
        return idx;
    }

    index_type get_or_add_ask_level(int64_t price) {
        index_type* indexed = price_index(ask_level_index_, price);
        if (indexed && *indexed != npos) return *indexed;

        index_type prev = npos;
        index_type cur = ask_head_;
        while (cur != npos && levels_[cur].price < price) {
            prev = cur;
            cur = levels_[cur].next;
        }
        if (cur != npos && levels_[cur].price == price) return cur;
        index_type idx = insert_level(SELL, price, prev, cur);
        if (indexed && idx != npos) *indexed = idx;
        return idx;
    }

    index_type insert_level(Side side, int64_t price, index_type prev, index_type next) {
        index_type idx = levels_.allocate();
        if (idx == npos) return npos;
        levels_[idx] = {price, 0, npos, npos, prev, next};

        if (prev != npos) levels_[prev].next = idx;
        else if (side == BUY) bid_head_ = idx;
        else ask_head_ = idx;

        if (next != npos) levels_[next].prev = idx;
        return idx;
    }

    void remove_order(index_type order_idx) {
        OrderNode& order = orders_[order_idx];
        LevelNode& level = levels_[order.level_idx];

        if (order.prev != npos) orders_[order.prev].next = order.next;
        else level.head = order.next;

        if (order.next != npos) orders_[order.next].prev = order.prev;
        else level.tail = order.prev;

        level.total_qty -= order.qty;
        cleanup_empty_level(order.side, order.level_idx);
    }

    void cleanup_empty_level(Side side, index_type level_idx) {
        LevelNode& level = levels_[level_idx];
        if (level.head != npos) return;

        if (level.prev != npos) levels_[level.prev].next = level.next;
        else if (side == BUY) bid_head_ = level.next;
        else ask_head_ = level.next;

        if (level.next != npos) levels_[level.next].prev = level.prev;
        index_type* indexed = price_index(side == BUY ? bid_level_index_ : ask_level_index_, level.price);
        if (indexed) *indexed = npos;
        levels_.release_unchecked(level_idx);
    }

    index_type* price_index(std::vector<index_type>& index, int64_t price) noexcept {
        if (index.empty() || price < price_base_) return nullptr;
        uint64_t offset = static_cast<uint64_t>(price - price_base_);
        if (offset >= index.size()) return nullptr;
        return &index[offset];
    }

    IntrusivePool<OrderNode> orders_;
    IntrusivePool<LevelNode> levels_;
    std::vector<index_type> order_index_;
    index_type bid_head_ = npos;
    index_type ask_head_ = npos;
    int64_t price_base_ = 0;
    std::vector<index_type> bid_level_index_;
    std::vector<index_type> ask_level_index_;
};
