#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <iostream>
#include <mutex>
#include <algorithm>

class SymbolManager {
public:
    static SymbolManager& instance() {
        static SymbolManager instance;
        return instance;
    }

    // 加载映射文件
    void load(const std::string& path) {
        std::lock_guard<std::mutex> lock(mtx_);
        std::ifstream file(path);
        if (!file.is_open()) {
            std::cerr << "[SymbolManager] Error: Cannot open " << path << std::endl;
            return;
        }

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            auto delim = line.find(':');
            if (delim == std::string::npos) continue;

            try {
                uint64_t id = std::stoull(line.substr(0, delim));
                std::string symbol = line.substr(delim + 1);
                
                // Trim potential whitespace
                symbol.erase(symbol.find_last_not_of(" \n\r\t") + 1);

                id_to_symbol_[id] = symbol;
                symbol_to_id_[symbol] = id;
            } catch (...) {
                continue;
            }
        }
        std::cout << "[SymbolManager] Loaded " << symbol_to_id_.size() << " symbols from " << path << std::endl;
    }

    // O(1) Lookup
    uint64_t get_id(const char* symbol) const {
        auto it = symbol_to_id_.find(symbol);
        if (it != symbol_to_id_.end()) {
            return it->second;
        }
        return 0; // 0 表示未知
    }

    // O(1) Lookup
    const char* get_symbol(uint64_t id) const {
        auto it = id_to_symbol_.find(id);
        if (it != id_to_symbol_.end()) {
            return it->second.c_str();
        }
        return "UNKNOWN";
    }

private:
    SymbolManager() = default;
    
    std::unordered_map<uint64_t, std::string> id_to_symbol_;
    std::unordered_map<std::string, uint64_t> symbol_to_id_;
    std::mutex mtx_;
};
