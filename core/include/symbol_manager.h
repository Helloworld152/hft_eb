#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <mutex>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <string_view>
#include "hash_containers.h"

class SymbolManager {
public:
    static SymbolManager& instance();

    // 加载映射文件
    void load(const std::string& path);

    // 恢复 const，数据由 Host 统一加载
    uint64_t get_id(std::string_view symbol) const;
    const char* get_symbol(uint64_t id) const;
    uint32_t get_index(uint64_t id) const;
    uint32_t get_index(std::string_view symbol) const;
    uint64_t get_symbol_id_by_index(uint32_t index) const;
    size_t symbol_count() const;

    // 合约乘数（未配置时默认 1.0）
    double get_multiplier(uint64_t id) const;
    double get_multiplier(std::string_view symbol) const;

    // 交易所管理
    void set_exchange(const std::string& symbol, const std::string& exchange);
    std::string get_exchange(const std::string& symbol) const;

private:
    SymbolManager();
    
    FastHashMap<uint64_t, std::string> id_to_symbol_;
    FastHashMap<std::string, uint64_t> symbol_to_id_;
    FastHashMap<uint64_t, double> id_to_multiplier_;
    FastHashMap<uint64_t, uint32_t> id_to_index_;
    std::vector<uint64_t> index_to_id_;
    FastHashMap<std::string, std::string> symbol_to_exchange_;
    mutable std::mutex mtx_;
    std::atomic<bool> loaded_;
};
