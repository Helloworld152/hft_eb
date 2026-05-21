#include "benchmark/benchmark.h"
#include "infra/include/queue.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kQueueCapacity = 1u << 16;
constexpr std::size_t kTotalOps = 1u << 20;

struct Msg64 {
    uint64_t seq = 0;
    unsigned char payload[56] = {};
};

static_assert(sizeof(Msg64) == 64, "Msg64 must be exactly 64 bytes");

inline void spin_pause() noexcept {
    __builtin_ia32_pause();
}

template <typename T>
void push_blocking(SpscQueue<T>& queue, const T& item) {
    while (!queue.push(item)) {
        spin_pause();
    }
}

template <typename T>
void push_blocking(MPMCRingBuffer<T>& queue, const T& item) {
    while (!queue.push(item)) {
        spin_pause();
    }
}

template <typename T>
void push_blocking(MPSCQueue<T>& queue, T item) {
    while (!queue.push(std::move(item))) {
        spin_pause();
    }
}

template <typename T>
void push_blocking(MPMCQueue<T>& queue, T item) {
    while (!queue.push(std::move(item))) {
        spin_pause();
    }
}

template <typename Queue>
void pop_blocking(Queue& queue, Msg64* item) {
    while (!queue.pop(*item)) {
        spin_pause();
    }
}

template <typename Queue>
static void BM_SingleProducerTransfer64(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        Queue queue(kQueueCapacity);
        std::atomic<int> ready{0};
        std::atomic<bool> start{false};
        std::atomic<uint64_t> checksum{0};

        std::thread consumer([&] {
            ready.fetch_add(1, std::memory_order_relaxed);
            while (!start.load(std::memory_order_acquire)) {
                spin_pause();
            }

            uint64_t local_sum = 0;
            Msg64 msg;
            for (std::size_t i = 0; i < kTotalOps; ++i) {
                pop_blocking(queue, &msg);
                local_sum += msg.seq;
            }
            checksum.store(local_sum, std::memory_order_relaxed);
        });

        std::thread producer([&] {
            ready.fetch_add(1, std::memory_order_relaxed);
            while (!start.load(std::memory_order_acquire)) {
                spin_pause();
            }

            for (std::size_t i = 0; i < kTotalOps; ++i) {
                Msg64 msg;
                msg.seq = static_cast<uint64_t>(i);
                push_blocking(queue, msg);
            }
        });

        while (ready.load(std::memory_order_acquire) != 2) {
            std::this_thread::yield();
        }

        state.ResumeTiming();
        const auto begin = std::chrono::steady_clock::now();
        start.store(true, std::memory_order_release);
        producer.join();
        consumer.join();
        const auto end = std::chrono::steady_clock::now();
        state.SetIterationTime(std::chrono::duration<double>(end - begin).count());
        benchmark::DoNotOptimize(checksum.load(std::memory_order_relaxed));
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kTotalOps));
}

static void BM_SpscClaimPublishTransfer64(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        SpscQueue<Msg64> queue(kQueueCapacity);
        std::atomic<int> ready{0};
        std::atomic<bool> start{false};
        std::atomic<uint64_t> checksum{0};

        std::thread consumer([&] {
            ready.fetch_add(1, std::memory_order_relaxed);
            while (!start.load(std::memory_order_acquire)) {
                spin_pause();
            }

            uint64_t local_sum = 0;
            for (std::size_t i = 0; i < kTotalOps; ++i) {
                Msg64* msg = nullptr;
                while ((msg = queue.peek()) == nullptr) {
                    spin_pause();
                }
                local_sum += msg->seq;
                queue.commit();
            }
            checksum.store(local_sum, std::memory_order_relaxed);
        });

        std::thread producer([&] {
            ready.fetch_add(1, std::memory_order_relaxed);
            while (!start.load(std::memory_order_acquire)) {
                spin_pause();
            }

            for (std::size_t i = 0; i < kTotalOps; ++i) {
                Msg64* msg = nullptr;
                while ((msg = queue.claim()) == nullptr) {
                    spin_pause();
                }
                msg->seq = static_cast<uint64_t>(i);
                queue.publish();
            }
        });

        while (ready.load(std::memory_order_acquire) != 2) {
            std::this_thread::yield();
        }

        state.ResumeTiming();
        const auto begin = std::chrono::steady_clock::now();
        start.store(true, std::memory_order_release);
        producer.join();
        consumer.join();
        const auto end = std::chrono::steady_clock::now();
        state.SetIterationTime(std::chrono::duration<double>(end - begin).count());
        benchmark::DoNotOptimize(checksum.load(std::memory_order_relaxed));
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kTotalOps));
}

template <typename Queue>
static void BM_MultiProducerTransfer64(benchmark::State& state) {
    const std::size_t producer_count = static_cast<std::size_t>(state.range(0));

    for (auto _ : state) {
        state.PauseTiming();
        Queue queue(kQueueCapacity);
        std::atomic<std::size_t> ready{0};
        std::atomic<bool> start{false};
        std::atomic<uint64_t> checksum{0};
        std::vector<std::thread> producers;
        producers.reserve(producer_count);

        const std::size_t base_ops = kTotalOps / producer_count;
        const std::size_t extra_ops = kTotalOps % producer_count;

        std::thread consumer([&] {
            ready.fetch_add(1, std::memory_order_relaxed);
            while (!start.load(std::memory_order_acquire)) {
                spin_pause();
            }

            uint64_t local_sum = 0;
            Msg64 msg;
            for (std::size_t i = 0; i < kTotalOps; ++i) {
                pop_blocking(queue, &msg);
                local_sum += msg.seq;
            }
            checksum.store(local_sum, std::memory_order_relaxed);
        });

        for (std::size_t producer_id = 0; producer_id < producer_count; ++producer_id) {
            producers.emplace_back([&, producer_id] {
                ready.fetch_add(1, std::memory_order_relaxed);
                while (!start.load(std::memory_order_acquire)) {
                    spin_pause();
                }

                const std::size_t ops = base_ops + (producer_id < extra_ops ? 1 : 0);
                const uint64_t base = static_cast<uint64_t>(producer_id) << 56;
                for (std::size_t i = 0; i < ops; ++i) {
                    Msg64 msg;
                    msg.seq = base | static_cast<uint64_t>(i);
                    push_blocking(queue, msg);
                }
            });
        }

        while (ready.load(std::memory_order_acquire) != producer_count + 1) {
            std::this_thread::yield();
        }

        state.ResumeTiming();
        const auto begin = std::chrono::steady_clock::now();
        start.store(true, std::memory_order_release);
        for (auto& producer : producers) {
            producer.join();
        }
        consumer.join();
        const auto end = std::chrono::steady_clock::now();
        state.SetIterationTime(std::chrono::duration<double>(end - begin).count());
        benchmark::DoNotOptimize(checksum.load(std::memory_order_relaxed));
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kTotalOps));
}

BENCHMARK_TEMPLATE(BM_SingleProducerTransfer64, SpscQueue<Msg64>)
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_SpscClaimPublishTransfer64)
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

BENCHMARK_TEMPLATE(BM_MultiProducerTransfer64, MPSCQueue<Msg64>)
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

BENCHMARK_TEMPLATE(BM_MultiProducerTransfer64, MPMCQueue<Msg64>)
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

BENCHMARK_TEMPLATE(BM_MultiProducerTransfer64, MPMCRingBuffer<Msg64>)
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

}  // namespace
