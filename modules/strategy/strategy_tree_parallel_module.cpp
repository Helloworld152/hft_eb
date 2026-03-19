#include "../../include/framework.h"
#include "../../core/include/ring_buffer.h"
#include "../../core/include/symbol_manager.h"

#include <dlfcn.h>
#include <yaml-cpp/yaml.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <immintrin.h>

// ------------------------------
// Local utilities (SPSC queue)
// ------------------------------
template <typename T>
class SpscQueue {
public:
    explicit SpscQueue(size_t capacity_pow2)
        : capacity_(round_up_pow2(capacity_pow2)),
          mask_(capacity_ - 1),
          buffer_(capacity_) {}

    bool push(const T& item) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        if (tail - head >= capacity_) return false;
        buffer_[tail & mask_] = item;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        if (head == tail) return false;
        item = buffer_[head & mask_];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

private:
    static size_t round_up_pow2(size_t v) {
        if (v < 2) return 2;
        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        if (sizeof(size_t) >= 8) v |= v >> 32;
        return v + 1;
    }

    const size_t capacity_;
    const size_t mask_;
    std::vector<T> buffer_;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_{0};
};

// ------------------------------
// Parallel Strategy Tree Module
// ------------------------------
class ParallelStrategyTreeModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config, ITimerService* timer_svc = nullptr) override {
        (void)timer_svc;
        bus_ = bus;

        if (config.find("publish_signals") != config.end()) {
            publish_signals_ = (config.at("publish_signals") == "true");
        }

        std::string yaml_content;
        if (config.find("_yaml") != config.end()) {
            yaml_content = config.at("_yaml");
        } else if (config.find("_json") != config.end()) {
            yaml_content = config.at("_json");
        } else {
            return;
        }

        YAML::Node doc;
        try {
            doc = YAML::Load(yaml_content);
        } catch (const YAML::Exception& e) {
            std::cerr << "[并行策略树] YAML解析失败: " << e.what() << std::endl;
            return;
        }

        // Module-level config
        if (doc["parallel"]) {
            parallel_enabled_ = doc["parallel"].as<bool>();
        }
        if (doc["shard_count"]) {
            shard_count_ = doc["shard_count"].as<size_t>();
        } else {
            shard_count_ = std::max<size_t>(1, std::thread::hardware_concurrency());
        }
        if (doc["queue_capacity"]) {
            queue_capacity_ = doc["queue_capacity"].as<size_t>();
        }
        if (doc["shard_by"]) {
            std::string shard_by = doc["shard_by"].as<std::string>();
            shard_by_ = (shard_by == "symbol") ? ShardBy::Symbol : ShardBy::SymbolId;
        }

        if (!parallel_enabled_) {
            shard_count_ = 1;
        }

        if (!doc["nodes"] || !doc["nodes"].IsSequence()) return;

        // Prepare shards
        shards_.clear();
        shards_.reserve(shard_count_);
        for (size_t i = 0; i < shard_count_; ++i) {
            shards_.emplace_back(queue_capacity_);
        }

        // Load nodes
        for (const auto& node_cfg : doc["nodes"]) {
            if (!node_cfg["id"] || !node_cfg["library"]) continue;

            std::string id = node_cfg["id"].as<std::string>();
            std::string lib_path = node_cfg["library"].as<std::string>();

            bool node_parallel = true;
            if (node_cfg["parallel"]) {
                node_parallel = node_cfg["parallel"].as<bool>();
            }
            if (node_cfg["role"]) {
                std::string role = node_cfg["role"].as<std::string>();
                if (role == "aggregator" || role == "strategy") node_parallel = false;
            }

            void* handle = dlopen(lib_path.c_str(), RTLD_LAZY);
            if (!handle) {
                std::cerr << "[并行策略树] 加载失败: " << lib_path << " | " << dlerror() << std::endl;
                continue;
            }

            CreateStrategyFunc create_fn = (CreateStrategyFunc)dlsym(handle, "create_strategy");
            if (!create_fn) {
                std::cerr << "[并行策略树] 符号未找到: create_strategy in " << lib_path << std::endl;
                dlclose(handle);
                continue;
            }

            // Build node config
            ConfigMap node_config;
            if (node_cfg["params"]) {
                if (node_cfg["params"].IsMap()) {
                    for (YAML::const_iterator it = node_cfg["params"].begin(); it != node_cfg["params"].end(); ++it) {
                        std::string key = it->first.as<std::string>();
                        if (it->second.IsScalar()) {
                            node_config[key] = it->second.as<std::string>();
                        }
                    }
                }

                YAML::Emitter out;
                out << node_cfg["params"];
                node_config["_yaml"] = out.c_str();
            }

            libs_.push_back({handle, id});

            if (node_parallel) {
                for (size_t s = 0; s < shard_count_; ++s) {
                    add_node_instance(id, create_fn, node_config, shards_[s].nodes);
                }
            } else {
                add_node_instance(id, create_fn, node_config, serial_nodes_);
            }
        }

        // Event routing
        bus_->subscribe(EVENT_MARKET_DATA, [this](void* d) {
            on_tick(static_cast<TickRecord*>(d));
            drain_signals();
        });

        bus_->subscribe(EVENT_KLINE, [this](void* d) {
            on_kline(static_cast<KlineRecord*>(d));
            drain_signals();
        });

        bus_->subscribe(EVENT_RTN_ORDER, [this](void* d) {
            on_order_update(static_cast<OrderRtn*>(d));
            drain_signals();
        });
    }

    void start() override {
        if (running_) return;
        running_ = true;
        for (auto& shard : shards_) {
            shard.worker = std::thread([this, &shard]() { shard_loop(shard); });
        }
    }

    void stop() override {
        if (!running_) return;
        running_ = false;
        for (auto& shard : shards_) {
            if (shard.worker.joinable()) shard.worker.join();
        }
        // Ensure no callbacks occur after nodes are destroyed
        serial_nodes_.clear();
        for (auto& shard : shards_) shard.nodes.clear();

        for (auto& lib : libs_) {
            if (lib.lib_handle) dlclose(lib.lib_handle);
        }
        libs_.clear();
    }

    ~ParallelStrategyTreeModule() override {
        stop();
    }

private:
    enum class ShardBy {
        SymbolId,
        Symbol
    };

    struct WorkItem {
        enum class Type : uint8_t { Tick, Kline, Signal, OrderUpdate };
        Type type;
        TickRecord tick;
        KlineRecord kline;
        SignalRecord signal;
        OrderRtn order;
    };

    struct NodeInstance {
        std::string id;
        std::unique_ptr<IStrategyNode> node;
        std::unique_ptr<StrategyContext> ctx;
    };

    struct ShardContext {
        explicit ShardContext(size_t queue_capacity) : queue(queue_capacity) {}
        SpscQueue<WorkItem> queue;
        std::vector<NodeInstance> nodes; // parallel nodes only
        std::thread worker;
    };

    struct LibHandle {
        void* lib_handle = nullptr;
        std::string id;
    };

    void add_node_instance(const std::string& id,
                           CreateStrategyFunc create_fn,
                           const ConfigMap& node_config,
                           std::vector<NodeInstance>& target) {
        IStrategyNode* strategy = create_fn();
        if (!strategy) return;

        auto ctx = std::make_unique<StrategyContext>();
        ctx->strategy_id = id;
        ctx->send_order = [this](const OrderReq& req) {
            bus_->publish(EVENT_ORDER_REQ, const_cast<OrderReq*>(&req));
        };
        ctx->send_signal = [this, id](const SignalRecord& sig) {
            SignalRecord internal_sig = sig;
            std::strncpy(internal_sig.source_id, id.c_str(), sizeof(internal_sig.source_id) - 1);
            enqueue_signal(internal_sig);
        };
        ctx->log = [id](const char* msg) {
            std::cout << "[策略-" << id << "] " << msg << std::endl;
        };

        strategy->init(ctx.get(), node_config);

        NodeInstance inst;
        inst.id = id;
        inst.node.reset(strategy);
        inst.ctx = std::move(ctx);
        target.push_back(std::move(inst));
    }

    void enqueue_signal(const SignalRecord& sig) {
        while (!signal_queue_.push(sig)) {
            _mm_pause();
        }
    }

    void drain_signals() {
        SignalRecord sig;
        while (signal_queue_.pop(sig)) {
            // 1) Send to parallel nodes (sharded by symbol)
            if (!shards_.empty()) {
                size_t shard = shard_for_symbol(sig.symbol);
                WorkItem item;
                item.type = WorkItem::Type::Signal;
                item.signal = sig;
                while (!shards_[shard].queue.push(item)) {
                    _mm_pause();
                }
            }

            // 2) Send to serial nodes immediately
            for (auto& n : serial_nodes_) {
                if (n.id == sig.source_id) continue;
                n.node->onSignal(&sig);
            }

            // 3) Optional publish to global bus
            if (publish_signals_) {
                bus_->publish(EVENT_SIGNAL, &sig);
            }
        }
    }

    void on_tick(const TickRecord* tick) {
        // Serial nodes first (single thread)
        for (auto& n : serial_nodes_) {
            n.node->onTick(tick);
        }

        if (shards_.empty()) return;

        WorkItem item;
        item.type = WorkItem::Type::Tick;
        item.tick = *tick;

        size_t shard = shard_for_tick(*tick);
        while (!shards_[shard].queue.push(item)) {
            _mm_pause();
        }
    }

    void on_kline(const KlineRecord* kline) {
        for (auto& n : serial_nodes_) {
            n.node->onKline(kline);
        }

        if (shards_.empty()) return;

        WorkItem item;
        item.type = WorkItem::Type::Kline;
        item.kline = *kline;
        size_t shard = shard_for_symbol(kline->symbol);
        while (!shards_[shard].queue.push(item)) {
            _mm_pause();
        }
    }

    void on_order_update(const OrderRtn* rtn) {
        for (auto& n : serial_nodes_) {
            n.node->onOrderUpdate(rtn);
        }

        if (shards_.empty()) return;

        WorkItem item;
        item.type = WorkItem::Type::OrderUpdate;
        item.order = *rtn;
        size_t shard = shard_for_symbol(rtn->symbol);
        while (!shards_[shard].queue.push(item)) {
            _mm_pause();
        }
    }

    void shard_loop(ShardContext& shard) {
        WorkItem item;
        while (running_) {
            if (!shard.queue.pop(item)) {
                _mm_pause();
                continue;
            }

            switch (item.type) {
                case WorkItem::Type::Tick:
                    for (auto& n : shard.nodes) {
                        n.node->onTick(&item.tick);
                    }
                    break;
                case WorkItem::Type::Kline:
                    for (auto& n : shard.nodes) {
                        n.node->onKline(&item.kline);
                    }
                    break;
                case WorkItem::Type::OrderUpdate:
                    for (auto& n : shard.nodes) {
                        n.node->onOrderUpdate(&item.order);
                    }
                    break;
                case WorkItem::Type::Signal:
                    for (auto& n : shard.nodes) {
                        if (n.id == item.signal.source_id) continue;
                        n.node->onSignal(&item.signal);
                    }
                    break;
            }
        }
    }

    size_t shard_for_tick(const TickRecord& tick) const {
        if (shard_by_ == ShardBy::SymbolId && tick.symbol_id != 0) {
            return tick.symbol_id % shards_.size();
        }
        return shard_for_symbol(tick.symbol);
    }

    size_t shard_for_symbol(const char* symbol) const {
        if (shards_.empty()) return 0;
        if (shard_by_ == ShardBy::SymbolId) {
            uint64_t id = SymbolManager::instance().get_id(symbol);
            if (id != 0) return id % shards_.size();
        }
        // FNV-1a 64-bit
        uint64_t hash = 1469598103934665603ull;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(symbol);
        while (*p) {
            hash ^= static_cast<uint64_t>(*p++);
            hash *= 1099511628211ull;
        }
        return static_cast<size_t>(hash % shards_.size());
    }

    EventBus* bus_ = nullptr;
    std::atomic<bool> running_{false};
    bool publish_signals_ = true;
    bool parallel_enabled_ = true;
    size_t shard_count_ = 1;
    size_t queue_capacity_ = 4096; // per-shard queue capacity
    ShardBy shard_by_ = ShardBy::SymbolId;

    std::vector<ShardContext> shards_;
    std::vector<NodeInstance> serial_nodes_;
    std::vector<LibHandle> libs_;

    static constexpr size_t SIGNAL_QUEUE_CAP = 65536;
    MPMCRingBuffer<SignalRecord, SIGNAL_QUEUE_CAP> signal_queue_;
};

EXPORT_MODULE(ParallelStrategyTreeModule)
