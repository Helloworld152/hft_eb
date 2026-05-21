#include "absl/container/flat_hash_map.h"
#include "benchmark/benchmark.h"

#include <cstdint>
#include <numeric>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using Key = uint64_t;
using StringKey = std::string;
using Value = uint64_t;

template <typename Keys>
Keys make_keys(std::size_t count, std::size_t base);

template <>
std::vector<Key> make_keys<std::vector<Key>>(std::size_t count, std::size_t base) {
    std::vector<Key> keys(count);
    std::iota(keys.begin(), keys.end(), static_cast<Key>(base));
    return keys;
}

template <>
std::vector<StringKey> make_keys<std::vector<StringKey>>(std::size_t count, std::size_t base) {
    std::vector<StringKey> keys;
    keys.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        keys.push_back("sym_" + std::to_string(base + i));
    }
    return keys;
}

struct StringHash {
    std::size_t operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }
    std::size_t operator()(const std::string& value) const noexcept {
        return (*this)(std::string_view(value));
    }
};

template <typename Map, typename Keys>
Map make_map(const Keys& keys) {
    Map map;
    map.reserve(keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        map.emplace(keys[i], static_cast<Value>(i));
    }
    return map;
}

template <typename Map, typename Keys>
static void BM_Insert(benchmark::State& state) {
    const Keys keys = make_keys<Keys>(static_cast<std::size_t>(state.range(0)), 1);
    for (auto _ : state) {
        Map map;
        map.reserve(keys.size());
        for (std::size_t i = 0; i < keys.size(); ++i) {
            benchmark::DoNotOptimize(map.emplace(keys[i], static_cast<Value>(i)));
        }
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

template <typename Map, typename Keys>
static void BM_FindHit(benchmark::State& state) {
    const Keys keys = make_keys<Keys>(static_cast<std::size_t>(state.range(0)), 1);
    const Map map = make_map<Map>(keys);

    for (auto _ : state) {
        for (const auto& key : keys) {
            auto it = map.find(key);
            Value value = it->second;
            benchmark::DoNotOptimize(it);
            benchmark::DoNotOptimize(value);
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

template <typename Map, typename Keys>
static void BM_FindMiss(benchmark::State& state) {
    const Keys keys = make_keys<Keys>(static_cast<std::size_t>(state.range(0)), 1);
    const Keys miss_keys = make_keys<Keys>(static_cast<std::size_t>(state.range(0)), 10'000'000);
    const Map map = make_map<Map>(keys);

    for (auto _ : state) {
        for (const auto& key : miss_keys) {
            benchmark::DoNotOptimize(map.find(key));
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

BENCHMARK_TEMPLATE(BM_Insert, std::unordered_map<Key, Value>, std::vector<Key>)
    ->RangeMultiplier(8)
    ->Range(1 << 10, 1 << 18);
BENCHMARK_TEMPLATE(BM_Insert, absl::flat_hash_map<Key, Value>, std::vector<Key>)
    ->RangeMultiplier(8)
    ->Range(1 << 10, 1 << 18);

BENCHMARK_TEMPLATE(BM_FindHit, std::unordered_map<Key, Value>, std::vector<Key>)
    ->RangeMultiplier(8)
    ->Range(1 << 10, 1 << 18);
BENCHMARK_TEMPLATE(BM_FindHit, absl::flat_hash_map<Key, Value>, std::vector<Key>)
    ->RangeMultiplier(8)
    ->Range(1 << 10, 1 << 18);

BENCHMARK_TEMPLATE(BM_FindMiss, std::unordered_map<Key, Value>, std::vector<Key>)
    ->RangeMultiplier(8)
    ->Range(1 << 10, 1 << 18);
BENCHMARK_TEMPLATE(BM_FindMiss, absl::flat_hash_map<Key, Value>, std::vector<Key>)
    ->RangeMultiplier(8)
    ->Range(1 << 10, 1 << 18);

BENCHMARK_TEMPLATE(BM_Insert, std::unordered_map<StringKey, Value, StringHash>, std::vector<StringKey>)
    ->RangeMultiplier(8)
    ->Range(1 << 10, 1 << 18);
BENCHMARK_TEMPLATE(BM_Insert, absl::flat_hash_map<StringKey, Value>, std::vector<StringKey>)
    ->RangeMultiplier(8)
    ->Range(1 << 10, 1 << 18);

BENCHMARK_TEMPLATE(BM_FindHit, std::unordered_map<StringKey, Value, StringHash>, std::vector<StringKey>)
    ->RangeMultiplier(8)
    ->Range(1 << 10, 1 << 18);
BENCHMARK_TEMPLATE(BM_FindHit, absl::flat_hash_map<StringKey, Value>, std::vector<StringKey>)
    ->RangeMultiplier(8)
    ->Range(1 << 10, 1 << 18);

BENCHMARK_TEMPLATE(BM_FindMiss, std::unordered_map<StringKey, Value, StringHash>, std::vector<StringKey>)
    ->RangeMultiplier(8)
    ->Range(1 << 10, 1 << 18);
BENCHMARK_TEMPLATE(BM_FindMiss, absl::flat_hash_map<StringKey, Value>, std::vector<StringKey>)
    ->RangeMultiplier(8)
    ->Range(1 << 10, 1 << 18);

}  // namespace
