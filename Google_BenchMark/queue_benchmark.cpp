#include "benchmark/benchmark.h"
#include "core/include/protocol.h"
#include "infra/include/queue.h"
#include <folly/ProducerConsumerQueue.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kQueueCapacity = 32u << 20;
constexpr std::size_t kTotalOps = 1u << 20;

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
struct QueueTraits {
    static Queue make(std::size_t capacity) {
        return Queue(capacity);
    }

    static void push(Queue& queue, TickRecord item) {
        push_blocking(queue, std::move(item));
    }

    static void pop(Queue& queue, TickRecord* item) {
        while (!queue.pop(*item)) {
            spin_pause();
        }
    }
};

template <typename T>
struct QueueTraits<folly::ProducerConsumerQueue<T>> {
    static folly::ProducerConsumerQueue<T> make(std::size_t capacity) {
        return folly::ProducerConsumerQueue<T>(static_cast<uint32_t>(capacity + 1));
    }

    static void push(folly::ProducerConsumerQueue<T>& queue, TickRecord item) {
        while (!queue.write(std::move(item))) {
            spin_pause();
        }
    }

    static void pop(folly::ProducerConsumerQueue<T>& queue, TickRecord* item) {
        while (!queue.read(*item)) {
            spin_pause();
        }
    }
};

template <typename Queue>
static void BM_SingleProducerTransfer64(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        auto queue = QueueTraits<Queue>::make(kQueueCapacity);
        std::atomic<int> ready{0};
        std::atomic<bool> start{false};
        std::atomic<uint64_t> checksum{0};

        std::thread consumer([&] {
            ready.fetch_add(1, std::memory_order_relaxed);
            while (!start.load(std::memory_order_acquire)) {
                spin_pause();
            }

            uint64_t local_sum = 0;
            TickRecord msg{};
            for (std::size_t i = 0; i < kTotalOps; ++i) {
                QueueTraits<Queue>::pop(queue, &msg);
                local_sum += msg.symbol_id;
            }
            checksum.store(local_sum, std::memory_order_relaxed);
        });

        std::thread producer([&] {
            ready.fetch_add(1, std::memory_order_relaxed);
            while (!start.load(std::memory_order_acquire)) {
                spin_pause();
            }

            for (std::size_t i = 0; i < kTotalOps; ++i) {
                TickRecord msg{};
                msg.symbol_id = static_cast<uint64_t>(i);
                QueueTraits<Queue>::push(queue, msg);
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
        SpscQueue<TickRecord> queue(kQueueCapacity);
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
                TickRecord* msg = nullptr;
                while ((msg = queue.peek()) == nullptr) {
                    spin_pause();
                }
                local_sum += msg->symbol_id;
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
                TickRecord* msg = nullptr;
                while ((msg = queue.claim()) == nullptr) {
                    spin_pause();
                }
                msg->symbol_id = static_cast<uint64_t>(i);
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
            TickRecord msg{};
            for (std::size_t i = 0; i < kTotalOps; ++i) {
                QueueTraits<Queue>::pop(queue, &msg);
                local_sum += msg.symbol_id;
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
                    TickRecord msg{};
                    msg.symbol_id = base | static_cast<uint64_t>(i);
                    QueueTraits<Queue>::push(queue, msg);
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

BENCHMARK_TEMPLATE(BM_SingleProducerTransfer64, SpscQueue<TickRecord>)
    ->Name("BM_SingleProducerTransfer64/SpscQueue")
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

BENCHMARK_TEMPLATE(BM_SingleProducerTransfer64, folly::ProducerConsumerQueue<TickRecord>)
    ->Name("BM_SingleProducerTransfer64/FollyProducerConsumerQueue")
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_SpscClaimPublishTransfer64)
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

BENCHMARK_TEMPLATE(BM_MultiProducerTransfer64, MPSCQueue<TickRecord>)
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

BENCHMARK_TEMPLATE(BM_MultiProducerTransfer64, MPMCQueue<TickRecord>)
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

BENCHMARK_TEMPLATE(BM_MultiProducerTransfer64, MPMCRingBuffer<TickRecord>)
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

}  // namespace
