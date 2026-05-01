#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <new>
#include <string>

std::atomic<size_t> g_new_calls{0};

void* operator new(std::size_t size) {
    g_new_calls.fetch_add(1, std::memory_order_relaxed);
    if (void* ptr = std::malloc(size)) {
        return ptr;
    }
    throw std::bad_alloc();
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void* operator new[](std::size_t size) {
    g_new_calls.fetch_add(1, std::memory_order_relaxed);
    if (void* ptr = std::malloc(size)) {
        return ptr;
    }
    throw std::bad_alloc();
}

void operator delete[](void* ptr) noexcept {
    std::free(ptr);
}

namespace {

struct BenchResult {
    double seconds;
    double ns_per_iter;
    uint64_t checksum;
};

template <typename Invoker>
BenchResult run_case(size_t iterations, Invoker&& invoke) {
    uint64_t acc = 1;
    const auto begin = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        acc = invoke(acc + static_cast<uint64_t>(i));
    }
    const auto end = std::chrono::steady_clock::now();

    volatile uint64_t sink = acc;
    (void)sink;

    const double seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(end - begin).count();
    const double ns_per_iter = seconds * 1e9 / static_cast<double>(iterations);
    return {seconds, ns_per_iter, acc};
}

void print_result(const std::string& name, const BenchResult& result, size_t iterations) {
    std::cout << std::left << std::setw(26) << name
              << " iterations=" << iterations
              << " time=" << std::fixed << std::setprecision(6) << result.seconds << "s"
              << " ns/op=" << std::setprecision(3) << result.ns_per_iter
              << " checksum=" << result.checksum << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    size_t iterations = 20'000'000;
    if (argc > 1) {
        iterations = static_cast<size_t>(std::strtoull(argv[1], nullptr, 10));
        if (iterations == 0) {
            std::cerr << "iterations must be > 0\n";
            return 1;
        }
    }

    std::array<uint64_t, 64> big{};
    for (size_t i = 0; i < big.size(); ++i) {
        big[i] = 0x9e3779b97f4a7c15ULL + static_cast<uint64_t>(i) * 0x100000001b3ULL;
    }

    const auto lambda = [big](uint64_t x) noexcept {
        return (x ^ big[0]) + big[1];
    };

    g_new_calls.store(0, std::memory_order_relaxed);
    std::function<uint64_t(uint64_t)> wrapped{lambda};
    const size_t wrapped_new_calls = g_new_calls.load(std::memory_order_relaxed);

    std::cout << "Lambda vs heap-backed std::function benchmark\n";
    std::cout << "iterations=" << iterations
              << " sizeof(lambda)=" << sizeof(lambda)
              << " new_calls_during_wrap=" << wrapped_new_calls << "\n\n";

    const auto direct = run_case(iterations, [&lambda](uint64_t value) noexcept {
        return lambda(value);
    });
    const auto wrapped_call = run_case(iterations, [&wrapped](uint64_t value) {
        return wrapped(value);
    });

    print_result("lambda_call", direct, iterations);
    print_result("std_function_call", wrapped_call, iterations);

    std::cout << "\nratio std::function/lambda="
              << std::fixed << std::setprecision(3)
              << (wrapped_call.ns_per_iter / direct.ns_per_iter) << "x\n";

    return wrapped_new_calls == 0 ? 2 : 0;
}
