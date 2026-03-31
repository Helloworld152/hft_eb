#include "../../include/framework.h"
#include "../../core/include/queue.h"
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
            shards_.push_back(std::make_unique<ShardContext>(queue_capacity_));
        }

        // Load nodes
        for (const auto& node_cfg : doc["nodes"]) {
            if (node_cfg["enabled"] && node_cfg["enabled"].as<bool>() == false) {
                continue;
            }
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
                    add_node_instance(id, create_fn, node_config, shards_[s]->nodes);
                }
            } else {
                add_node_instance(id, create_fn, node_config, serial_nodes_);
            }
        }

        // Event routing
        bus_->subscribe(EVENT_MARKET_DATA, [this](void* d) {
            on_tick(static_cast<TickRecord*>(d));
        });

        bus_->subscribe(EVENT_KLINE, [this](void* d) {
            on_kline(static_cast<KlineRecord*>(d));
        });

        bus_->subscribe(EVENT_RTN_ORDER, [this](void* d) {
            on_order_update(static_cast<OrderRtn*>(d));
        });
    }

    void start() override {
        if (running_) return;
        running_ = true;
        for (auto& shard : shards_) {
            auto* shard_ptr = shard.get();
            shard->worker = std::thread([this, shard_ptr]() { shard_loop(*shard_ptr); });
        }
    }

    void stop() override {
        if (!running_) return;
        running_ = false;
        for (auto& shard : shards_) {
            if (shard->worker.joinable()) shard->worker.join();
        }
        // Ensure no callbacks occur after nodes are destroyed
        serial_nodes_.clear();
        for (auto& shard : shards_) shard->nodes.clear();

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
        enum class Type : uint8_t { Tick, Kline, OrderUpdate };
        Type type;
        TickRecord tick;
        KlineRecord kline;
        OrderRtn order;
    };

    struct NodeInstance {
        std::string id;
        std::unique_ptr<IStrategyNode> node;
        std::unique_ptr<StrategyContext> ctx;
    };

    struct ShardContext {
        explicit ShardContext(size_t queue_capacity)
            : work_queue(queue_capacity) {}
        SpscQueue<WorkItem> work_queue;   // Tick/Kline/OrderUpdate (single producer: event thread)
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
        while (running_ && !signal_queue_.push(sig)) {
            _mm_pause();
        }
    }

    void drain_signals() {
        SignalRecord sig;
        size_t drained = 0;
        while (drained < SIGNAL_DRAIN_BATCH && signal_queue_.pop(sig)) {
            for (auto& n : serial_nodes_) {
                if (n.id == sig.source_id) continue;
                n.node->onSignal(&sig);
            }
            if (publish_signals_) {
                bus_->publish(EVENT_SIGNAL, &sig);
            }
            ++drained;
        }
    }


    void on_tick(const TickRecord* tick) {
        if (!running_) return;
        drain_signals();
        // Serial nodes first (single thread)
        for (auto& n : serial_nodes_) {
            n.node->onTick(tick);
        }

        if (shards_.empty()) return;

        WorkItem item;
        item.type = WorkItem::Type::Tick;
        item.tick = *tick;

        size_t shard = shard_for_tick(*tick);
        while (running_ && !shards_[shard]->work_queue.push(item)) {
            _mm_pause();
        }
    }

    void on_kline(const KlineRecord* kline) {
        if (!running_) return;
        for (auto& n : serial_nodes_) {
            n.node->onKline(kline);
        }

        if (shards_.empty()) return;

        WorkItem item;
        item.type = WorkItem::Type::Kline;
        item.kline = *kline;
        size_t shard = shard_for_symbol(kline->symbol);
        while (running_ && !shards_[shard]->work_queue.push(item)) {
            _mm_pause();
        }
    }

    void on_order_update(const OrderRtn* rtn) {
        if (!running_) return;
        for (auto& n : serial_nodes_) {
            n.node->onOrderUpdate(rtn);
        }

        if (shards_.empty()) return;

        WorkItem item;
        item.type = WorkItem::Type::OrderUpdate;
        item.order = *rtn;
        size_t shard = shard_for_symbol(rtn->symbol);
        while (running_ && !shards_[shard]->work_queue.push(item)) {
            _mm_pause();
        }
    }

    void shard_loop(ShardContext& shard) {
        WorkItem item;
        while (running_) {
            if (shard.work_queue.pop(item)) {
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
                }
                continue;
            }
            _mm_pause();
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

    std::vector<std::unique_ptr<ShardContext>> shards_;
    std::vector<NodeInstance> serial_nodes_;
    std::vector<LibHandle> libs_;

    static constexpr size_t SIGNAL_DRAIN_BATCH = 64;
    static constexpr size_t SIGNAL_QUEUE_CAP = 65536;
    MPMCRingBuffer<SignalRecord> signal_queue_{SIGNAL_QUEUE_CAP};
};

EXPORT_MODULE(ParallelStrategyTreeModule)
