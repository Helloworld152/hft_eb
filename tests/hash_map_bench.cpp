#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct TimingResult {
    const char* name;
    double insert_ms;
    double hit_lookup_ms;
    double miss_lookup_ms;
    double erase_ms;
};

template <typename Fn>
double measure_ms(Fn&& fn) {
    const auto begin = Clock::now();
    fn();
    const auto end = Clock::now();
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

std::vector<uint64_t> make_int_keys(size_t count) {
    std::vector<uint64_t> keys(count);
    std::iota(keys.begin(), keys.end(), 1ULL);
    return keys;
}

std::vector<std::string> make_string_keys(size_t count) {
    std::vector<std::string> keys;
    keys.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        keys.push_back("rb" + std::to_string(10'000'000 + i));
    }
    return keys;
}

template <typename T>
void rotate_keys(std::vector<T>* keys) {
    if (keys->empty()) return;
    std::rotate(keys->begin(), keys->begin() + (keys->size() / 3), keys->end());
}

template <typename Map, typename Key>
TimingResult bench_map(const char* name,
                       const std::vector<Key>& keys,
                       const std::vector<Key>& miss_keys) {
    volatile uint64_t sink = 0;

    double insert_ms = measure_ms([&] {
        Map map;
        map.reserve(keys.size());
        for (size_t i = 0; i < keys.size(); ++i) {
            map.emplace(keys[i], static_cast<uint64_t>(i));
        }
        sink += map.size();
    });

    Map map;
    map.reserve(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        map.emplace(keys[i], static_cast<uint64_t>(i));
    }

    double hit_lookup_ms = measure_ms([&] {
        uint64_t local = 0;
        for (const auto& key : keys) {
            auto it = map.find(key);
            if (it != map.end()) {
                local += it->second;
            }
        }
        sink += local;
    });

    double miss_lookup_ms = measure_ms([&] {
        uint64_t local = 0;
        for (const auto& key : miss_keys) {
            local += static_cast<uint64_t>(map.find(key) == map.end());
        }
        sink += local;
    });

    double erase_ms = measure_ms([&] {
        for (const auto& key : keys) {
            map.erase(key);
        }
        sink += map.size();
    });

    return {name, insert_ms, hit_lookup_ms, miss_lookup_ms, erase_ms};
}

template <typename Set, typename Key>
TimingResult bench_set(const char* name,
                       const std::vector<Key>& keys,
                       const std::vector<Key>& miss_keys) {
    volatile uint64_t sink = 0;

    double insert_ms = measure_ms([&] {
        Set set;
        set.reserve(keys.size());
        for (const auto& key : keys) {
            set.emplace(key);
        }
        sink += set.size();
    });

    Set set;
    set.reserve(keys.size());
    for (const auto& key : keys) {
        set.emplace(key);
    }

    double hit_lookup_ms = measure_ms([&] {
        uint64_t local = 0;
        for (const auto& key : keys) {
            local += static_cast<uint64_t>(set.find(key) != set.end());
        }
        sink += local;
    });

    double miss_lookup_ms = measure_ms([&] {
        uint64_t local = 0;
        for (const auto& key : miss_keys) {
            local += static_cast<uint64_t>(set.find(key) == set.end());
        }
        sink += local;
    });

    double erase_ms = measure_ms([&] {
        for (const auto& key : keys) {
            set.erase(key);
        }
        sink += set.size();
    });

    return {name, insert_ms, hit_lookup_ms, miss_lookup_ms, erase_ms};
}

template <typename RootMap>
double bench_nested_root(const std::vector<std::string>& accounts,
                         const std::vector<uint64_t>& symbols) {
    volatile uint64_t sink = 0;
    return measure_ms([&] {
        RootMap root;
        root.reserve(accounts.size());
        for (const auto& account : accounts) {
            auto& inner = root[account];
            inner.reserve(symbols.size());
            for (size_t i = 0; i < symbols.size(); ++i) {
                inner.emplace(symbols[i], static_cast<uint64_t>(i));
            }
        }

        uint64_t local = 0;
        for (const auto& account : accounts) {
            auto it_root = root.find(account);
            if (it_root == root.end()) continue;
            for (const auto& symbol : symbols) {
                auto it_inner = it_root->second.find(symbol);
                if (it_inner != it_root->second.end()) {
                    local += it_inner->second;
                }
            }
        }
        sink += local;
    });
}

void print_result(const char* title, const TimingResult& std_res, const TimingResult& absl_res) {
    auto ratio = [](double std_ms, double absl_ms) {
        return absl_ms > 0.0 ? std_ms / absl_ms : 0.0;
    };

    std::cout << "\n[" << title << "]\n";
    std::cout << std::left << std::setw(18) << "container"
              << std::right << std::setw(12) << "insert"
              << std::setw(12) << "hit"
              << std::setw(12) << "miss"
              << std::setw(12) << "erase" << '\n';
    std::cout << std::left << std::setw(18) << std_res.name
              << std::right << std::fixed << std::setprecision(3)
              << std::setw(12) << std_res.insert_ms
              << std::setw(12) << std_res.hit_lookup_ms
              << std::setw(12) << std_res.miss_lookup_ms
              << std::setw(12) << std_res.erase_ms << '\n';
    std::cout << std::left << std::setw(18) << absl_res.name
              << std::right << std::fixed << std::setprecision(3)
              << std::setw(12) << absl_res.insert_ms
              << std::setw(12) << absl_res.hit_lookup_ms
              << std::setw(12) << absl_res.miss_lookup_ms
              << std::setw(12) << absl_res.erase_ms << '\n';
    std::cout << std::left << std::setw(18) << "std/absl ratio"
              << std::right << std::fixed << std::setprecision(2)
              << std::setw(12) << ratio(std_res.insert_ms, absl_res.insert_ms)
              << std::setw(12) << ratio(std_res.hit_lookup_ms, absl_res.hit_lookup_ms)
              << std::setw(12) << ratio(std_res.miss_lookup_ms, absl_res.miss_lookup_ms)
              << std::setw(12) << ratio(std_res.erase_ms, absl_res.erase_ms) << '\n';
}

}  // namespace

int main() {
    constexpr size_t kMapSize = 200'000;
    constexpr size_t kAccountCount = 32;
    constexpr size_t kSymbolsPerAccount = 4'096;

    auto int_keys = make_int_keys(kMapSize);
    auto int_miss_keys = make_int_keys(kMapSize);
    for (auto& key : int_miss_keys) key += 10 * kMapSize;
    rotate_keys(&int_keys);
    rotate_keys(&int_miss_keys);

    auto string_keys = make_string_keys(kMapSize);
    auto string_miss_keys = make_string_keys(kMapSize);
    for (auto& key : string_miss_keys) key += "_miss";
    rotate_keys(&string_keys);
    rotate_keys(&string_miss_keys);

    auto std_int_map = bench_map<std::unordered_map<uint64_t, uint64_t>>(
        "std::unordered_map", int_keys, int_miss_keys);
    auto absl_int_map = bench_map<absl::flat_hash_map<uint64_t, uint64_t>>(
        "absl::flat_hash_map", int_keys, int_miss_keys);
    print_result("uint64_t -> uint64_t map", std_int_map, absl_int_map);

    auto std_str_map = bench_map<std::unordered_map<std::string, uint64_t>>(
        "std::unordered_map", string_keys, string_miss_keys);
    auto absl_str_map = bench_map<absl::flat_hash_map<std::string, uint64_t>>(
        "absl::flat_hash_map", string_keys, string_miss_keys);
    print_result("string -> uint64_t map", std_str_map, absl_str_map);

    auto std_int_set = bench_set<std::unordered_set<uint64_t>>(
        "std::unordered_set", int_keys, int_miss_keys);
    auto absl_int_set = bench_set<absl::flat_hash_set<uint64_t>>(
        "absl::flat_hash_set", int_keys, int_miss_keys);
    print_result("uint64_t set", std_int_set, absl_int_set);

    std::vector<std::string> accounts;
    accounts.reserve(kAccountCount);
    for (size_t i = 0; i < kAccountCount; ++i) {
        accounts.push_back("acc_" + std::to_string(i));
    }
    auto nested_symbols = make_int_keys(kSymbolsPerAccount);
    rotate_keys(&nested_symbols);

    using StdInner = std::unordered_map<uint64_t, uint64_t>;
    using StdRoot = std::unordered_map<std::string, StdInner>;
    using AbslInner = absl::flat_hash_map<uint64_t, uint64_t>;
    using AbslRoot = absl::flat_hash_map<std::string, AbslInner>;

    const double std_nested_ms = bench_nested_root<StdRoot>(accounts, nested_symbols);
    const double absl_nested_ms = bench_nested_root<AbslRoot>(accounts, nested_symbols);

    std::cout << "\n[nested account -> symbol map]\n";
    std::cout << "std::unordered_map total_ms=" << std::fixed << std::setprecision(3)
              << std_nested_ms << '\n';
    std::cout << "absl::flat_hash_map total_ms=" << std::fixed << std::setprecision(3)
              << absl_nested_ms << '\n';
    std::cout << "std/absl ratio=" << std::setprecision(2)
              << (absl_nested_ms > 0.0 ? std_nested_ms / absl_nested_ms : 0.0) << "x\n";

    return 0;
}
