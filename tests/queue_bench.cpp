#include "core/include/queue.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

struct BenchResult {
    double seconds;
    double mops;
};

struct BenchStats {
    double avg_mops;
    double best_mops;
};

template <typename T>
class SpscQueueAdapter {
public:
    explicit SpscQueueAdapter(size_t capacity) : queue_(capacity) {}

    bool push(T&& item) noexcept {
        while (!queue_.push(item)) {
            __builtin_ia32_pause();
        }
        return true;
    }

    bool pop(T& item) noexcept {
        while (!queue_.pop(item)) {
            __builtin_ia32_pause();
        }
        return true;
    }

private:
    SpscQueue<T> queue_;
};

template <typename Queue>
Queue make_queue(size_t capacity) {
    if constexpr (std::is_constructible_v<Queue, size_t>) {
        return Queue(capacity);
    } else {
        return Queue();
    }
}

template <typename Queue, typename T>
void push_blocking(Queue& queue, T&& item) {
    while (!queue.push(std::forward<T>(item))) {
        __builtin_ia32_pause();
    }
}

template <typename Queue, typename T>
void pop_blocking(Queue& queue, T& item) {
    while (!queue.pop(item)) {
        __builtin_ia32_pause();
    }
}

template <typename Queue>
BenchResult run_bench(const std::string& name,
                      size_t producer_count,
                      size_t total_ops,
                      size_t capacity) {
    Queue queue = make_queue<Queue>(capacity);
    std::atomic<size_t> ready{0};
    std::atomic<bool> start{false};
    std::atomic<uint64_t> checksum{0};

    std::vector<std::thread> producers;
    producers.reserve(producer_count);

    const size_t base_ops = total_ops / producer_count;
    const size_t extra_ops = total_ops % producer_count;

    auto consumer = std::thread([&] {
        ready.fetch_add(1, std::memory_order_relaxed);
        while (!start.load(std::memory_order_acquire)) {
            __builtin_ia32_pause();
        }

        uint64_t local_sum = 0;
        for (size_t i = 0; i < total_ops; ++i) {
            uint64_t value = 0;
            pop_blocking(queue, value);
            local_sum += value;
        }
        checksum.store(local_sum, std::memory_order_relaxed);
    });

    for (size_t p = 0; p < producer_count; ++p) {
        producers.emplace_back([&, p] {
            ready.fetch_add(1, std::memory_order_relaxed);
            while (!start.load(std::memory_order_acquire)) {
                __builtin_ia32_pause();
            }

            const size_t ops = base_ops + (p < extra_ops ? 1 : 0);
            uint64_t value = static_cast<uint64_t>(p) << 48;
            for (size_t i = 0; i < ops; ++i) {
                uint64_t item = value + static_cast<uint64_t>(i);
                push_blocking(queue, std::move(item));
            }
        });
    }

    while (ready.load(std::memory_order_acquire) != producer_count + 1) {
        std::this_thread::yield();
    }

    const auto begin = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);

    for (auto& producer : producers) {
        producer.join();
    }
    consumer.join();

    const auto end = std::chrono::steady_clock::now();
    const double seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(end - begin).count();
    const double mops = static_cast<double>(total_ops) / seconds / 1'000'000.0;

    std::cout << std::left << std::setw(10) << name
              << " producers=" << std::setw(2) << producer_count
              << " ops=" << total_ops
              << " cap=" << capacity
              << " time=" << std::fixed << std::setprecision(6) << seconds << "s"
              << " throughput=" << std::setprecision(2) << mops << " Mops/s"
              << " checksum=" << checksum.load(std::memory_order_relaxed)
              << '\n';

    return {seconds, mops};
}

template <typename Queue>
BenchStats run_bench_series(const std::string& name,
                            size_t producer_count,
                            size_t total_ops,
                            size_t capacity,
                            size_t repeats) {
    double total_mops = 0.0;
    double best_mops = 0.0;

    for (size_t i = 0; i < repeats; ++i) {
        auto result = run_bench<Queue>(name, producer_count, total_ops, capacity);
        total_mops += result.mops;
        if (result.mops > best_mops) {
            best_mops = result.mops;
        }
    }

    return {total_mops / static_cast<double>(repeats), best_mops};
}

}  // namespace

int main() {
    constexpr size_t capacity = 1u << 16;
    constexpr size_t total_ops = 4u * 1000u * 1000u;
    constexpr size_t repeats = 5;
    const std::vector<size_t> producer_counts{1, 2, 4, 8};

    std::cout << "Queue benchmark under MPSC workload\n";
    std::cout << "total_ops=" << total_ops
              << " capacity=" << capacity
              << " repeats=" << repeats << "\n\n";

    for (size_t producers : producer_counts) {
        auto mpsc =
            run_bench_series<MPSCQueue<uint64_t>>("MPSC", producers, total_ops, capacity, repeats);
        auto mpmc =
            run_bench_series<MPMCQueue<uint64_t>>("MPMC", producers, total_ops, capacity, repeats);
        auto ring =
            run_bench_series<MPMCRingBuffer<uint64_t>>("Ring", producers, total_ops, capacity, repeats);
        std::cout << "summary producers=" << producers
                  << " MPSC(avg/best)=" << std::fixed << std::setprecision(2)
                  << mpsc.avg_mops << "/" << mpsc.best_mops << " Mops/s"
                  << " MPMC(avg/best)=" << mpmc.avg_mops << "/" << mpmc.best_mops << " Mops/s"
                  << " Ring(avg/best)=" << ring.avg_mops << "/" << ring.best_mops << " Mops/s\n";
        std::cout << "ratio avg MPSC/MPMC="
                  << std::fixed << std::setprecision(2)
                  << (mpsc.avg_mops / mpmc.avg_mops) << "x"
                  << " best MPSC/MPMC=" << (mpsc.best_mops / mpmc.best_mops) << "x"
                  << " avg Ring/MPMC=" << (ring.avg_mops / mpmc.avg_mops) << "x"
                  << " best Ring/MPMC=" << (ring.best_mops / mpmc.best_mops) << "x\n\n";
    }

    std::cout << "Single-producer comparison against SPSC\n";
    std::cout << "total_ops=" << total_ops
              << " capacity=" << capacity
              << " repeats=" << repeats << "\n\n";

    auto spsc =
        run_bench_series<SpscQueueAdapter<uint64_t>>("SPSC", 1, total_ops, capacity, repeats);
    auto mpsc_single =
        run_bench_series<MPSCQueue<uint64_t>>("MPSC", 1, total_ops, capacity, repeats);
    auto mpmc_single =
        run_bench_series<MPMCQueue<uint64_t>>("MPMC", 1, total_ops, capacity, repeats);
    auto ring_single =
        run_bench_series<MPMCRingBuffer<uint64_t>>("Ring", 1, total_ops, capacity, repeats);

    std::cout << "summary single-producer"
              << " SPSC(avg/best)=" << std::fixed << std::setprecision(2)
              << spsc.avg_mops << "/" << spsc.best_mops << " Mops/s"
              << " MPSC(avg/best)=" << mpsc_single.avg_mops << "/" << mpsc_single.best_mops
              << " Mops/s"
              << " MPMC(avg/best)=" << mpmc_single.avg_mops << "/" << mpmc_single.best_mops
              << " Mops/s"
              << " Ring(avg/best)=" << ring_single.avg_mops << "/" << ring_single.best_mops
              << " Mops/s\n";
    std::cout << "ratio avg MPSC/SPSC=" << (mpsc_single.avg_mops / spsc.avg_mops) << "x"
              << " MPMC/SPSC=" << (mpmc_single.avg_mops / spsc.avg_mops) << "x"
              << " Ring/SPSC=" << (ring_single.avg_mops / spsc.avg_mops) << "x"
              << " best MPSC/SPSC=" << (mpsc_single.best_mops / spsc.best_mops) << "x"
              << " best MPMC/SPSC=" << (mpmc_single.best_mops / spsc.best_mops) << "x"
              << " best Ring/SPSC=" << (ring_single.best_mops / spsc.best_mops) << "x\n";

    return 0;
}
