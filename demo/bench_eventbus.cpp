#include <iostream>
#include <vector>
#include <functional>
#include <chrono>
#include <memory>
#include <array>

// ==========================================
// 0. Payload
// ==========================================
struct TickRecord {
    double price;
    int volume;
};

// Global volatile to prevent optimization
volatile double g_sink = 0;

// The target function logic
void onTick(void* data) {
    TickRecord* t = static_cast<TickRecord*>(data);
    g_sink += t->price;
}

struct Receiver {
    void onTick(void* data) {
        TickRecord* t = static_cast<TickRecord*>(data);
        g_sink += t->price;
    }
};

// ==========================================
// 1. Direct Static Call
// ==========================================
// Simulating a hardcoded dispatcher
struct StaticDispatcher {
    Receiver* r;
    void dispatch(void* data) {
        r->onTick(data);
    }
};

// ==========================================
// 2. std::function Direct
// ==========================================
// Simulating holding a std::function directly
struct StdFuncDispatcher {
    std::function<void(void*)> func;
    void dispatch(void* data) {
        func(data);
    }
};

// ==========================================
// 3. Naive EventBus (std::vector<std::function>)
// ==========================================
struct NaiveEventBus {
    using Handler = std::function<void(void*)>;
    std::vector<Handler> handlers;

    void subscribe(Handler h) {
        handlers.push_back(h);
    }

    void publish(void* data) {
        for (auto& h : handlers) {
            h(data);
        }
    }
};

// ==========================================
// 4. Current Optimized EventBus (Copied from engine.cpp)
// ==========================================
struct FastHandler {
    void (*func)(void* context, void* data);
    void* context;
};

constexpr size_t MAX_HANDLERS_PER_EVENT = 32;
struct alignas(64) EventSlot {
    FastHandler handlers[MAX_HANDLERS_PER_EVENT];
    uint8_t count = 0;
    char padding[7];
};

struct LambdaWrapper {
    std::function<void(void*)> func;
    explicit LambdaWrapper(std::function<void(void*)> f) : func(std::move(f)) {}
    static void invoke(void* ctx, void* data) {
        static_cast<LambdaWrapper*>(ctx)->func(data);
    }
};

struct OptimizedEventBus {
    EventSlot slot;
    std::vector<std::unique_ptr<LambdaWrapper>> storage;

    void subscribe(std::function<void(void*)> handler) {
        if (slot.count < MAX_HANDLERS_PER_EVENT) {
            auto wrapper = std::make_unique<LambdaWrapper>(std::move(handler));
            slot.handlers[slot.count] = {LambdaWrapper::invoke, wrapper.get()};
            slot.count++;
            storage.push_back(std::move(wrapper));
        }
    }

    void publish(void* data) {
        const uint8_t cnt = slot.count;
        uint8_t i = 0;
        // Unrolled loop as in engine.cpp
        while (i + 4 <= cnt) {
            slot.handlers[i].func(slot.handlers[i].context, data);
            slot.handlers[i+1].func(slot.handlers[i+1].context, data);
            slot.handlers[i+2].func(slot.handlers[i+2].context, data);
            slot.handlers[i+3].func(slot.handlers[i+3].context, data);
            i += 4;
        }
        while (i < cnt) {
            slot.handlers[i].func(slot.handlers[i].context, data);
            i++;
        }
    }
};

// ==========================================
// 5. Optimized EventBus with RAW Function Pointer (No std::function)
// ==========================================
// This tests if we remove std::function wrapper layer
struct RawFastHandler {
    void (*func)(void* data);
};
struct RawEventBus {
    RawFastHandler handlers[32];
    int count = 0;
    
    void subscribe(void (*f)(void*)) {
        handlers[count++] = {f};
    }
    
    void publish(void* data) {
        for(int i=0; i<count; ++i) {
            handlers[i].func(data);
        }
    }
};


// ==========================================
// Main Benchmark
// ==========================================
int main() {
    const int ITERATIONS = 100000000; // 100 Million
    TickRecord tick{100.0, 1};
    Receiver receiver;

    std::cout << "Benchmarking " << ITERATIONS << " iterations..." << std::endl;

    // 1. Static Direct Call
    {
        StaticDispatcher d{&receiver};
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < ITERATIONS; ++i) {
            d.dispatch(&tick);
        }
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::nano> ns = end - start;
        std::cout << "[Static Call]       Avg: " << ns.count() / ITERATIONS << " ns" << std::endl;
    }

    // 2. std::function Direct
    {
        StdFuncDispatcher d;
        d.func = std::bind(&Receiver::onTick, &receiver, std::placeholders::_1);
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < ITERATIONS; ++i) {
            d.dispatch(&tick);
        }
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::nano> ns = end - start;
        std::cout << "[std::function]     Avg: " << ns.count() / ITERATIONS << " ns" << std::endl;
    }

    // 3. Naive EventBus (vector<std::function>)
    {
        NaiveEventBus bus;
        bus.subscribe(std::bind(&Receiver::onTick, &receiver, std::placeholders::_1));
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < ITERATIONS; ++i) {
            bus.publish(&tick);
        }
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::nano> ns = end - start;
        std::cout << "[Naive EventBus]    Avg: " << ns.count() / ITERATIONS << " ns" << std::endl;
    }

    // 4. Current Optimized EventBus
    {
        OptimizedEventBus bus;
        bus.subscribe(std::bind(&Receiver::onTick, &receiver, std::placeholders::_1));
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < ITERATIONS; ++i) {
            bus.publish(&tick);
        }
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::nano> ns = end - start;
        std::cout << "[Current Optimized] Avg: " << ns.count() / ITERATIONS << " ns" << std::endl;
    }
    
    // 5. Raw EventBus (Function Pointer)
    {
        RawEventBus bus;
        bus.subscribe(onTick); // Must use free function or static method
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < ITERATIONS; ++i) {
            bus.publish(&tick);
        }
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::nano> ns = end - start;
        std::cout << "[Raw Func Ptr Bus]  Avg: " << ns.count() / ITERATIONS << " ns" << std::endl;
    }

    std::cout << "\n--- Multi-Handler Test (4 handlers) ---" << std::endl;

    // 6. Naive EventBus (4 handlers)
    {
        NaiveEventBus bus;
        for(int k=0; k<4; ++k) bus.subscribe(std::bind(&Receiver::onTick, &receiver, std::placeholders::_1));
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < ITERATIONS; ++i) {
            bus.publish(&tick);
        }
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::nano> ns = end - start;
        std::cout << "[Naive EventBus x4] Avg: " << ns.count() / ITERATIONS << " ns" << std::endl;
    }

    // 7. Current Optimized EventBus (4 handlers)
    {
        OptimizedEventBus bus;
        for(int k=0; k<4; ++k) bus.subscribe(std::bind(&Receiver::onTick, &receiver, std::placeholders::_1));
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < ITERATIONS; ++i) {
            bus.publish(&tick);
        }
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::nano> ns = end - start;
        std::cout << "[Current Opt x4]    Avg: " << ns.count() / ITERATIONS << " ns" << std::endl;
    }

    return 0;
}
