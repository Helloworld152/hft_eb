#include "../include/symbol_manager.h"
#include <iostream>
#include <filesystem>

SymbolManager& SymbolManager::instance() {
    static SymbolManager instance;
    return instance;
}

SymbolManager::SymbolManager() : loaded_(false) {}

void SymbolManager::load(const std::string& path) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (loaded_.load()) return;

    std::string final_path = path;
    if (!std::filesystem::exists(final_path)) {
        if (std::filesystem::exists("../" + path)) {
            final_path = "../" + path;
        } else if (std::filesystem::exists("./bin/" + path)) {
            final_path = "./bin/" + path;
        }
    }

    std::ifstream file(final_path);
    if (!file.is_open()) {
        std::cerr << "[SymbolManager] ERROR: Cannot open symbols file at " << final_path << std::endl;
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto first = line.find(':');
        if (first == std::string::npos) continue;

        try {
            uint64_t id = std::stoull(line.substr(0, first));
            std::string rest = line.substr(first + 1);
            auto second = rest.find(':');
            std::string symbol = (second == std::string::npos) ? rest : rest.substr(0, second);
            symbol.erase(symbol.find_last_not_of(" \n\r\t") + 1);

            double multiplier = 1.0;
            if (second != std::string::npos) {
                std::string mul_str = rest.substr(second + 1);
                mul_str.erase(0, mul_str.find_first_not_of(" \t"));
                mul_str.erase(mul_str.find_last_not_of(" \n\r\t") + 1);
                if (!mul_str.empty())
                    multiplier = std::stod(mul_str);
            }

            id_to_symbol_[id] = symbol;
            symbol_to_id_[symbol] = id;
            id_to_multiplier_[id] = multiplier;
        } catch (...) {
            continue;
        }
    }
    loaded_.store(true);
    std::cout << "[SymbolManager] Loaded " << symbol_to_id_.size() << " symbols." << std::endl;
}

uint64_t SymbolManager::get_id(const char* symbol) const {
    auto it = symbol_to_id_.find(symbol);
    if (it != symbol_to_id_.end()) {
        return it->second;
    }
    return 0;
}

const char* SymbolManager::get_symbol(uint64_t id) const {
    auto it = id_to_symbol_.find(id);
    if (it != id_to_symbol_.end()) {
        return it->second.c_str();
    }
    return "UNKNOWN";
}

double SymbolManager::get_multiplier(uint64_t id) const {
    auto it = id_to_multiplier_.find(id);
    if (it != id_to_multiplier_.end())
        return it->second;
    return 1.0;
}

double SymbolManager::get_multiplier(const char* symbol) const {
    uint64_t id = get_id(symbol);
    return id ? get_multiplier(id) : 1.0;
}
