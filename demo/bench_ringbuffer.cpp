#include "../core/include/ring_buffer.h"
#include "../core/include/protocol.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include <iomanip>
#include <immintrin.h> 
#include <sched.h>     
#include <algorithm>

constexpr size_t OPS_COUNT = 50000000; // 50 Million
constexpr size_t BUFFER_SIZE = 65536;
constexpr int ITERATIONS = 3;

struct MockTick {
    uint64_t id;
    double price;
    char padding[48]; 
};

void set_affinity(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

void bench_batch_zerocopy(int iter, size_t batch_size) {
    BatchRingBuffer<MockTick, BUFFER_SIZE> rb;
    std::atomic<bool> start{false};

    auto producer = [&]() {
        set_affinity(1);
        while (!start.load(std::memory_order_acquire));
        size_t remaining = OPS_COUNT;
        while (remaining > 0) {
            auto [ptr, len] = rb.reserve();
            if (len > 0) {
                size_t to_write = std::min({len, batch_size, remaining});
                for (size_t k=0; k<to_write; ++k) ptr[k].id = remaining;
                rb.commit(to_write);
                remaining -= to_write;
            } else { _mm_pause(); }
        }
    };

    auto consumer = [&]() {
        set_affinity(2);
        while (!start.load(std::memory_order_acquire));
        size_t remaining = OPS_COUNT;
        while (remaining > 0) {
            auto [ptr, len] = rb.peek();
            if (len > 0) {
                size_t to_read = std::min(len, remaining);
                for (size_t k=0; k<to_read; ++k) {
                    volatile uint64_t val = ptr[k].id; (void)val;
                }
                rb.advance(to_read);
                remaining -= to_read;
            } else { _mm_pause(); }
        }
    };

    std::thread t1(producer);
    std::thread t2(consumer);
    auto start_time = std::chrono::high_resolution_clock::now();
    start.store(true, std::memory_order_release);
    t1.join(); t2.join();
    auto end_time = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double>(end_time - start_time).count();
    std::cout << "[Batch " << std::setw(4) << batch_size << "] Iter " << iter << ": " 
              << std::fixed << std::setprecision(2) << (OPS_COUNT / duration / 1e6) << " Mops/sec" << std::endl;
}

int main() {
    std::cout << "Sweep Test: Different Batch Sizes (" << OPS_COUNT << " msgs)" << std::endl;
    for(size_t b : {1, 32, 128, 512, 2048}) {
        for(int i=1; i<=ITERATIONS; ++i) bench_batch_zerocopy(i, b);
        std::cout << "---" << std::endl;
    }
    return 0;
}