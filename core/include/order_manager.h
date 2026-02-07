#pragma once

#include <cstdint>
#include <string>
#include <atomic>
#include <chrono>
#include "protocol.h"

// 订单上下文，记录订单全生命周期
struct OrderContext {
    OrderReq request;
    char order_ref[13]{0};
    char order_sys_id[21]{0};
    int filled_volume{0};
    char status{'3'}; // 默认 '3': 未成交/已报
    uint64_t insert_time{0};
    uint64_t update_time{0};
};

class OrderIDGenerator {
public:
    static OrderIDGenerator& instance() {
        static OrderIDGenerator inst;
        return inst;
    }

    void set_node_id(uint32_t node_id) {
        node_id_ = node_id & 0x3FF; // 10 bit
    }

    uint64_t next_id() {
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        struct tm* lt = std::localtime(&now);
        
        // YYMMDDHHMMSS (12 digits)
        uint64_t time_part = static_cast<uint64_t>(lt->tm_year % 100) * 10000000000ULL + 
                             static_cast<uint64_t>(lt->tm_mon + 1) * 100000000ULL + 
                             static_cast<uint64_t>(lt->tm_mday) * 1000000ULL + 
                             static_cast<uint64_t>(lt->tm_hour) * 10000ULL + 
                             static_cast<uint64_t>(lt->tm_min) * 100ULL + 
                             static_cast<uint64_t>(lt->tm_sec);
        
        // Seq: 0-9999 (4 digits)
        uint32_t seq = sequence_.fetch_add(1, std::memory_order_relaxed) % 10000;
        
        // Result: YYMMDDHHMMSS NN SSSS (18 digits total)
        // fits in uint64_t (max ~1.8e19)
        return time_part * 1000000ULL + 
               static_cast<uint64_t>(node_id_ % 100) * 10000ULL + 
               static_cast<uint64_t>(seq);
    }

    // 生成用于柜台映射的 ID (12位数字字符串，兼容 CTP OrderRef)
    void next_order_ref(char* out) {
        uint32_t ref = ref_sequence_.fetch_add(1, std::memory_order_relaxed);
        snprintf(out, 13, "%012u", ref);
    }

    void set_start_ref(uint32_t start_ref) {
        uint32_t current = ref_sequence_.load();
        while (start_ref > current && !ref_sequence_.compare_exchange_weak(current, start_ref)) {
            // 继续尝试直到成功或当前值已经更大
        }
    }

private:
    OrderIDGenerator() : node_id_(0), sequence_(0), ref_sequence_(1) {}
    uint32_t node_id_;
    std::atomic<uint32_t> sequence_;
    std::atomic<uint32_t> ref_sequence_;
};
