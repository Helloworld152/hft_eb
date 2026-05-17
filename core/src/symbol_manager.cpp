#include "symbol_manager.h"
#include "logging.h"
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
        LOG_ERROR("[SymbolManager] Cannot open symbols file at {}", final_path);
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

            auto id_it = id_to_symbol_.find(id);
            if (id_it != id_to_symbol_.end() && id_it->second != symbol) {
                LOG_ERROR("[SymbolManager] Duplicate symbol id {} for '{}' and '{}', skip later entry.",
                          id,
                          id_it->second,
                          symbol);
                continue;
            }

            auto symbol_it = symbol_to_id_.find(symbol);
            if (symbol_it != symbol_to_id_.end() && symbol_it->second != id) {
                LOG_ERROR("[SymbolManager] Duplicate symbol '{}' for ids {} and {}, skip later entry.",
                          symbol,
                          symbol_it->second,
                          id);
                continue;
            }

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
            if (id_to_index_.find(id) == id_to_index_.end()) {
                uint32_t index = static_cast<uint32_t>(index_to_id_.size());
                id_to_index_[id] = index;
                index_to_id_.push_back(id);
            }
        } catch (...) {
            continue;
        }
    }
    loaded_.store(true);
    LOG_INFO("[SymbolManager] Loaded {} symbols.", symbol_to_id_.size());
}

uint64_t SymbolManager::get_id(std::string_view symbol) const {
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

uint32_t SymbolManager::get_index(uint64_t id) const {
    auto it = id_to_index_.find(id);
    if (it != id_to_index_.end()) {
        return it->second;
    }
    return std::numeric_limits<uint32_t>::max();
}

uint32_t SymbolManager::get_index(std::string_view symbol) const {
    uint64_t id = get_id(symbol);
    return id ? get_index(id) : std::numeric_limits<uint32_t>::max();
}

uint64_t SymbolManager::get_symbol_id_by_index(uint32_t index) const {
    if (index < index_to_id_.size()) {
        return index_to_id_[index];
    }
    return 0;
}

size_t SymbolManager::symbol_count() const {
    return index_to_id_.size();
}

double SymbolManager::get_multiplier(uint64_t id) const {
    auto it = id_to_multiplier_.find(id);
    if (it != id_to_multiplier_.end())
        return it->second;
    return 1.0;
}

double SymbolManager::get_multiplier(std::string_view symbol) const {
    uint64_t id = get_id(symbol);
    return id ? get_multiplier(id) : 1.0;
}
