#include "../core/include/ring_buffer.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include <iomanip>
#include <immintrin.h> 
#include <sched.h>     
#include <cstring>
#include <algorithm>

constexpr size_t OPS_COUNT = 20000000; // 20 Million
constexpr size_t BUFFER_SIZE = 65536;

// 64 Bytes (1 Cache Line)
struct Tick64 {
    uint64_t id;
    char data[56];
};

// 256 Bytes (4 Cache Lines)
struct Tick256 {
    uint64_t id;
    char data[248];
};

// 1024 Bytes (16 Cache Lines)
struct Tick1024 {
    uint64_t id;
    char data[1016];
};

void set_affinity(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

template <typename T>
void bench_payload(const std::string& name) {
    auto rb_ptr = std::make_unique<BatchRingBuffer<T, BUFFER_SIZE>>();
    BatchRingBuffer<T, BUFFER_SIZE>& rb = *rb_ptr;
    std::atomic<bool> start{false};
    constexpr size_t BATCH = 512; // High throughput batching

    auto producer = [&]() {
        set_affinity(1);
        while (!start.load(std::memory_order_acquire));
        
        T sample;
        memset(&sample, 0xAA, sizeof(T));

        size_t remaining = OPS_COUNT;
        while (remaining > 0) {
            auto [ptr, len] = rb.reserve();
            if (len > 0) {
                size_t to_write = std::min({len, BATCH, remaining});
                for (size_t k=0; k<to_write; ++k) {
                    // Simulate real data arrival: Copy from IO buffer directly into RingBuffer
                    // This is ONE full write.
                    memcpy(&ptr[k], &sample, sizeof(T));
                    ptr[k].id = remaining;
                }
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
                    // Simulate real read: Access start and end to force cache loading
                    volatile uint64_t v1 = ptr[k].id;
                    volatile char v2 = ptr[k].data[sizeof(T::data)-1];
                    (void)v1; (void)v2;
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
    
    double mops = OPS_COUNT / duration / 1e6;
    double bw_gb = mops * sizeof(T) / 1024.0; // GB/s (approx)
    
    std::cout << "[" << std::setw(8) << name << "] " 
              << std::setw(4) << sizeof(T) << " Bytes | "
              << std::fixed << std::setprecision(2) << mops << " Mops/sec | "
              << "~" << bw_gb << " GB/s Bandwidth (ZeroCopy)" << std::endl;
}

template <typename T>
void bench_basic_payload(const std::string& name) {
    auto rb_ptr = std::make_unique<RingBuffer<T, BUFFER_SIZE>>();
    RingBuffer<T, BUFFER_SIZE>& rb = *rb_ptr;
    std::atomic<bool> start{false};

    auto producer = [&]() {
        set_affinity(1);
        while (!start.load(std::memory_order_acquire));
        
        T sample;
        memset(&sample, 0xAA, sizeof(T));

        for (size_t i = 0; i < OPS_COUNT; ++i) {
            sample.id = i;
            while (!rb.push(sample)) _mm_pause(); // Implicit memcpy 1
        }
    };

    auto consumer = [&]() {
        set_affinity(2);
        while (!start.load(std::memory_order_acquire));
        T temp;
        for (size_t i = 0; i < OPS_COUNT; ++i) {
            while (!rb.pop(temp)) _mm_pause(); // Implicit memcpy 2
            
            // Access to force cache load (fair comparison)
            volatile uint64_t v1 = temp.id;
            volatile char v2 = temp.data[sizeof(T::data)-1];
            (void)v1; (void)v2;
        }
    };

    std::thread t1(producer);
    std::thread t2(consumer);
    auto start_time = std::chrono::high_resolution_clock::now();
    start.store(true, std::memory_order_release);
    t1.join(); t2.join();
    auto end_time = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double>(end_time - start_time).count();
    
    double mops = OPS_COUNT / duration / 1e6;
    double bw_gb = mops * sizeof(T) / 1024.0; 
    
    std::cout << "[Basic " << std::setw(8) << name << "] " 
              << std::setw(4) << sizeof(T) << " Bytes | "
              << std::fixed << std::setprecision(2) << mops << " Mops/sec | "
              << "~" << bw_gb << " GB/s Bandwidth (Copy)" << std::endl;
}

int main() {
    std::cout << "Benchmarking Zero-Copy vs Copy (20000000 msgs, 5 Iterations)..." << std::endl;
    
    for (int i = 1; i <= 5; ++i) {
        std::cout << "=== Iteration " << i << " ===" << std::endl;
        std::cout << "--- Zero Copy (Batch=512) ---" << std::endl;
        bench_payload<Tick64>("Tick64");
        bench_payload<Tick256>("Tick256");
        bench_payload<Tick1024>("Tick1024");

        std::cout << "--- Basic Copy ---" << std::endl;
        bench_basic_payload<Tick64>("Tick64");
        bench_basic_payload<Tick256>("Tick256");
        bench_basic_payload<Tick1024>("Tick1024");
        std::cout << std::endl;
    }
    
    return 0;
}
