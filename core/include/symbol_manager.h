#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <iostream>
#include <mutex>
#include <algorithm>
#include <atomic>

class SymbolManager {
public:
    static SymbolManager& instance();

    // 加载映射文件
    void load(const std::string& path);

    // 恢复 const，数据由 Host 统一加载
    uint64_t get_id(const char* symbol) const;
    const char* get_symbol(uint64_t id) const;

    // 新增：交易所管理
    void set_exchange(const std::string& symbol, const std::string& exchange);
    std::string get_exchange(const std::string& symbol) const;

private:
    SymbolManager();
    
    std::unordered_map<uint64_t, std::string> id_to_symbol_;
    std::unordered_map<std::string, uint64_t> symbol_to_id_;
    std::unordered_map<std::string, std::string> symbol_to_exchange_; // symbol -> exchange_id
    mutable std::mutex mtx_;
    std::atomic<bool> loaded_;
};