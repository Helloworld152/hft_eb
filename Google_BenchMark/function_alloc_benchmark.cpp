#include "benchmark/benchmark.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <new>

// 2026-05-23, WSL2 / Release / --benchmark_min_time=0.2s
//
// 直接调用:
// - 直接 lambda（小对象，无捕获）:                0.203 ns
// - std::function（小对象，不触发堆分配）:        0.832 ns
// - 直接 lambda（捕获 64B）:                     0.205 ns
// - std::function（捕获 64B，触发堆分配）:       0.822 ns
//
// 构造:
// - std::function（小对象，不触发堆分配）:        1.04 ns   alloc_per_construct=0
// - std::function（捕获 64B，触发堆分配）:       8.78 ns   alloc_per_construct=1
//
// 复制后再调用（更贴近运行时按值传递/复制场景）:
// - 直接 lambda（小对象） copy+call:             0.208 ns
// - std::function（小对象） copy+call:          3.26 ns
// - 直接 lambda（捕获 64B） copy+call:          0.619 ns
// - std::function（捕获 64B） copy+call:        9.90 ns
//
// 结论:
// - 单纯调用时，lambda 与 std::function 的差距主要来自 type erasure，本机约 4x。
// - 触发堆分配后，std::function 的主要损耗在构造/复制，不在单次调用。
// - 一旦进入 copy+call 场景，大捕获 std::function 的堆分配成本会被明显放大。

namespace {

std::atomic<std::uint64_t> g_alloc_count{0};

void* alloc_bytes(std::size_t size) {
    if (void* ptr = std::malloc(size)) {
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
        return ptr;
    }
    throw std::bad_alloc();
}

struct Capture64 {
    std::array<std::uint64_t, 8> words{};

    int operator()(int x) const noexcept {
        return x + static_cast<int>(words[0]);
    }
};

template <typename Factory>
double allocations_per_construct(Factory&& factory, std::size_t samples) {
    const std::uint64_t before = g_alloc_count.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < samples; ++i) {
        auto fn = factory();
        benchmark::DoNotOptimize(fn);
    }
    const std::uint64_t after = g_alloc_count.load(std::memory_order_relaxed);
    return static_cast<double>(after - before) / static_cast<double>(samples);
}

static void BM_DirectLambdaCall(benchmark::State& state) {
    auto lambda = [](int x) noexcept { return x + 1; };
    int value = 0;
    for (auto _ : state) {
        value = lambda(value);
        benchmark::DoNotOptimize(value);
    }
}

static void BM_DirectLambdaCallCapture64B(benchmark::State& state) {
    auto lambda = [cap = Capture64{}](int x) noexcept {
        return x + static_cast<int>(cap.words[0]) + 1;
    };
    int value = 0;
    for (auto _ : state) {
        value = lambda(value);
        benchmark::DoNotOptimize(value);
    }
}

static void BM_StdFunctionCallNoAlloc(benchmark::State& state) {
    std::function<int(int)> fn = [](int x) noexcept { return x + 1; };
    int value = 0;
    for (auto _ : state) {
        value = fn(value);
        benchmark::DoNotOptimize(value);
    }
    state.counters["alloc_per_construct"] = allocations_per_construct(
        [] { return std::function<int(int)>([](int x) noexcept { return x + 1; }); },
        1024);
}

static void BM_StdFunctionCallHeapAlloc64B(benchmark::State& state) {
    std::function<int(int)> fn = Capture64{};
    int value = 0;
    for (auto _ : state) {
        value = fn(value);
        benchmark::DoNotOptimize(value);
    }
    state.counters["alloc_per_construct"] =
        allocations_per_construct([] { return std::function<int(int)>(Capture64{}); }, 1024);
}

static void BM_StdFunctionConstructNoAlloc(benchmark::State& state) {
    for (auto _ : state) {
        std::function<int(int)> fn = [](int x) noexcept { return x + 1; };
        benchmark::DoNotOptimize(fn);
        benchmark::ClobberMemory();
    }
    state.counters["alloc_per_construct"] = allocations_per_construct(
        [] { return std::function<int(int)>([](int x) noexcept { return x + 1; }); },
        1024);
}

static void BM_StdFunctionConstructHeapAlloc64B(benchmark::State& state) {
    for (auto _ : state) {
        std::function<int(int)> fn = Capture64{};
        benchmark::DoNotOptimize(fn);
        benchmark::ClobberMemory();
    }
    state.counters["alloc_per_construct"] =
        allocations_per_construct([] { return std::function<int(int)>(Capture64{}); }, 1024);
}

static void BM_DirectLambdaCopyCall(benchmark::State& state) {
    auto lambda = [](int x) noexcept { return x + 1; };
    int value = 0;
    for (auto _ : state) {
        auto copied = lambda;
        benchmark::DoNotOptimize(copied);
        value = copied(value);
        benchmark::DoNotOptimize(value);
        benchmark::ClobberMemory();
    }
}

static void BM_DirectLambdaCopyCallCapture64B(benchmark::State& state) {
    auto lambda = [cap = Capture64{}](int x) noexcept {
        return x + static_cast<int>(cap.words[0]) + 1;
    };
    int value = 0;
    for (auto _ : state) {
        auto copied = lambda;
        benchmark::DoNotOptimize(copied);
        value = copied(value);
        benchmark::DoNotOptimize(value);
        benchmark::ClobberMemory();
    }
}

static void BM_StdFunctionCopyCallNoAlloc(benchmark::State& state) {
    std::function<int(int)> fn = [](int x) noexcept { return x + 1; };
    int value = 0;
    for (auto _ : state) {
        auto copied = fn;
        benchmark::DoNotOptimize(copied);
        value = copied(value);
        benchmark::DoNotOptimize(value);
        benchmark::ClobberMemory();
    }
}

static void BM_StdFunctionCopyCallHeapAlloc64B(benchmark::State& state) {
    std::function<int(int)> fn = Capture64{};
    int value = 0;
    for (auto _ : state) {
        auto copied = fn;
        benchmark::DoNotOptimize(copied);
        value = copied(value);
        benchmark::DoNotOptimize(value);
        benchmark::ClobberMemory();
    }
}

BENCHMARK(BM_DirectLambdaCall);
BENCHMARK(BM_DirectLambdaCallCapture64B);
BENCHMARK(BM_StdFunctionCallNoAlloc);
BENCHMARK(BM_StdFunctionCallHeapAlloc64B);
BENCHMARK(BM_StdFunctionConstructNoAlloc);
BENCHMARK(BM_StdFunctionConstructHeapAlloc64B);
BENCHMARK(BM_DirectLambdaCopyCall);
BENCHMARK(BM_DirectLambdaCopyCallCapture64B);
BENCHMARK(BM_StdFunctionCopyCallNoAlloc);
BENCHMARK(BM_StdFunctionCopyCallHeapAlloc64B);

}  // namespace

void* operator new(std::size_t size) {
    return alloc_bytes(size);
}

void* operator new[](std::size_t size) {
    return alloc_bytes(size);
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void operator delete[](void* ptr) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
    std::free(ptr);
}
