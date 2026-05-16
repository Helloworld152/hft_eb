#include "framework.h"
#include "factor/factor_node.h"
#include "symbol_manager.h"
#include <yaml-cpp/yaml.h>
#include <dlfcn.h>
#include <vector>
#include <memory>
#include <cstring>

struct FactorNodeHandle {
    void* lib_handle = nullptr;
    std::unique_ptr<IFactorNode> node;
    std::string id;
    uint64_t symbol_id = 0;
    bool has_symbol = false;
    bool dirty = true;
    double cached_value = 0.0;
    std::vector<FactorNodeHandle*> children;
    std::vector<double> inputs_cache;

    ~FactorNodeHandle() {
        node.reset();
        if (lib_handle) {
            dlclose(lib_handle);
        }
    }
};

struct OutputSpec {
    FactorNodeHandle* node = nullptr;
    std::string factor_name;
    bool has_symbol = false;
    uint64_t symbol_id = 0;
    char cached_symbol[32] = {0};
    char cached_factor_name[32] = {0};
    char cached_source_id[32] = {0};
};

class FactorDAGModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config, ITimerService* timer_svc = nullptr) override {
        bus_ = bus;
        timer_svc_ = timer_svc;

        if (config.find("_yaml") == config.end()) {
            LOG_ERROR("[FactorDAG] Missing _yaml config. Skip init.");
            return;
        }

        YAML::Node doc;
        try {
            doc = YAML::Load(config.at("_yaml"));
        } catch (const YAML::Exception& e) {
            LOG_ERROR("[FactorDAG] YAML parse error: {}", e.what());
            return;
        }

        parse_nodes(doc);
        parse_edges(doc);
        parse_outputs(doc);
        parse_trigger(doc);

        if (trigger_on_tick_ || trigger_on_timer_) {
            bus_->subscribe(EVENT_MARKET_DATA,
                            StaticDelegate<void(void*)>::bind<FactorDAGModule, &FactorDAGModule::on_tick_event>(this));
        }
        if (trigger_on_kline_ || trigger_on_timer_) {
            bus_->subscribe(EVENT_KLINE,
                            StaticDelegate<void(void*)>::bind<FactorDAGModule, &FactorDAGModule::on_kline_event>(this));
        }
        if (trigger_on_timer_) {
            if (timer_svc_) {
                timer_svc_->add_timer(timer_interval_sec_,
                                      StaticDelegate<void()>::bind<FactorDAGModule, &FactorDAGModule::on_timer>(this),
                                      0);
            } else {
                LOG_ERROR("[FactorDAG] Timer trigger enabled but timer service is null.");
            }
        }
    }

private:
    void on_tick_event(void* d) {
        on_tick(static_cast<TickRecord*>(d));
    }

    void on_kline_event(void* d) {
        on_kline(static_cast<KlineRecord*>(d));
    }
    void parse_nodes(const YAML::Node& doc) {
        if (!doc["nodes"] || !doc["nodes"].IsSequence()) {
            LOG_ERROR("[FactorDAG] No nodes configured.");
            return;
        }

        for (const auto& node_cfg : doc["nodes"]) {
            if (!node_cfg["id"] || !node_cfg["library"]) continue;

            std::string id = node_cfg["id"].as<std::string>();
            std::string lib_path = node_cfg["library"].as<std::string>();

            void* handle = dlopen(lib_path.c_str(), RTLD_LAZY);
            if (!handle) {
                LOG_ERROR("[FactorDAG] dlopen failed: {} | {}", lib_path, dlerror());
                continue;
            }

            CreateFactorFunc create_fn = (CreateFactorFunc)dlsym(handle, "create_factor");
            if (!create_fn) {
                LOG_ERROR("[FactorDAG] create_factor not found in: {}", lib_path);
                dlclose(handle);
                continue;
            }

            IFactorNode* factor = create_fn();
            if (!factor) {
                LOG_ERROR("[FactorDAG] create_factor returned null: {}", lib_path);
                dlclose(handle);
                continue;
            }

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

            factor->init(node_config);

            auto handle_ptr = std::make_unique<FactorNodeHandle>();
            handle_ptr->lib_handle = handle;
            handle_ptr->node = std::unique_ptr<IFactorNode>(factor);
            handle_ptr->id = id;

            auto it = node_config.find("symbol");
            if (it != node_config.end()) {
                handle_ptr->symbol_id = SymbolManager::instance().get_id(it->second.c_str());
                handle_ptr->has_symbol = (handle_ptr->symbol_id != 0);
            }

            nodes_.emplace(id, std::move(handle_ptr));
        }
    }

    void parse_edges(const YAML::Node& doc) {
        if (!doc["edges"] || !doc["edges"].IsSequence()) return;

        for (const auto& edge : doc["edges"]) {
            if (!edge["from"] || !edge["to"]) continue;
            std::string from = edge["from"].as<std::string>();
            std::string to = edge["to"].as<std::string>();

            auto it_child = nodes_.find(from);
            auto it_parent = nodes_.find(to);
            if (it_child == nodes_.end() || it_parent == nodes_.end()) {
                LOG_WARN("[FactorDAG] Edge refers to missing node: {} -> {}", from, to);
                continue;
            }
            it_parent->second->children.push_back(it_child->second.get());
        }
    }

    void parse_outputs(const YAML::Node& doc) {
        if (!doc["outputs"] || !doc["outputs"].IsSequence()) return;

        for (const auto& out : doc["outputs"]) {
            if (!out["node"]) continue;
            std::string node_id = out["node"].as<std::string>();
            auto it = nodes_.find(node_id);
            if (it == nodes_.end()) {
                LOG_WARN("[FactorDAG] Output refers to missing node: {}", node_id);
                continue;
            }
            OutputSpec spec;
            spec.node = it->second.get();
            if (out["factor_name"]) {
                spec.factor_name = out["factor_name"].as<std::string>();
            } else {
                spec.factor_name = node_id;
            }
            spec.has_symbol = spec.node->has_symbol;
            spec.symbol_id = spec.node->symbol_id;
            std::strncpy(spec.cached_source_id, spec.node->id.c_str(), sizeof(spec.cached_source_id) - 1);
            std::strncpy(spec.cached_factor_name, spec.factor_name.c_str(), sizeof(spec.cached_factor_name) - 1);
            if (spec.has_symbol && spec.symbol_id != 0) {
                const char* sym = SymbolManager::instance().get_symbol(spec.symbol_id);
                if (sym) {
                    std::strncpy(spec.cached_symbol, sym, sizeof(spec.cached_symbol) - 1);
                }
            }
            outputs_.push_back(std::move(spec));
        }
    }

    void parse_trigger(const YAML::Node& doc) {
        if (!doc["trigger"] || !doc["trigger"].IsMap()) return;
        const auto& tr = doc["trigger"];
        if (tr["on_tick"]) trigger_on_tick_ = tr["on_tick"].as<bool>();
        if (tr["on_kline"]) trigger_on_kline_ = tr["on_kline"].as<bool>();
        if (tr["on_timer"]) trigger_on_timer_ = tr["on_timer"].as<bool>();
        if (tr["timer_interval_sec"]) {
            int v = tr["timer_interval_sec"].as<int>();
            if (v > 0) timer_interval_sec_ = v;
        }
    }

    void on_tick(const TickRecord* tick) {
        if (!tick) return;
        last_tick_by_id_[tick->symbol_id] = *tick;
        current_tick_id_ = tick->symbol_id;
        mark_dirty_for_symbol(tick->symbol_id);
        if (trigger_on_tick_) {
            eval_and_publish();
        }
    }

    void on_kline(const KlineRecord* kline) {
        if (!kline) return;
        last_kline_by_id_[kline->symbol_id] = *kline;
        current_kline_id_ = kline->symbol_id;
        mark_dirty_for_symbol(kline->symbol_id);
        if (trigger_on_kline_) {
            eval_and_publish();
        }
    }

    void on_timer() {
        eval_and_publish();
    }

    void mark_dirty_for_symbol(uint64_t symbol_id) {
        for (auto& kv : nodes_) {
            auto& n = kv.second;
            if (!n->has_symbol || n->symbol_id == symbol_id) {
                n->dirty = true;
            }
        }
    }

    double eval_node(FactorNodeHandle* node) {
        if (!node) return 0.0;
        if (!node->dirty) return node->cached_value;

        std::vector<double>& inputs = node->inputs_cache;
        if (inputs.capacity() < node->children.size()) {
            inputs.reserve(node->children.size());
        }
        inputs.clear();
        for (auto* child : node->children) {
            inputs.push_back(eval_node(child));
        }

        FactorContext ctx;
        ctx.tick = get_tick_for_node(node);
        ctx.kline = get_kline_for_node(node);

        node->cached_value = node->node->compute(ctx, inputs);
        node->dirty = false;
        return node->cached_value;
    }

    const TickRecord* get_tick_for_node(const FactorNodeHandle* node) const {
        if (node && node->has_symbol) {
            auto it = last_tick_by_id_.find(node->symbol_id);
            if (it != last_tick_by_id_.end()) return &it->second;
            return nullptr;
        }
        auto it = last_tick_by_id_.find(current_tick_id_);
        if (it != last_tick_by_id_.end()) return &it->second;
        return nullptr;
    }

    const KlineRecord* get_kline_for_node(const FactorNodeHandle* node) const {
        if (node && node->has_symbol) {
            auto it = last_kline_by_id_.find(node->symbol_id);
            if (it != last_kline_by_id_.end()) return &it->second;
            return nullptr;
        }
        auto it = last_kline_by_id_.find(current_kline_id_);
        if (it != last_kline_by_id_.end()) return &it->second;
        return nullptr;
    }

    void eval_and_publish() {
        for (const auto& out : outputs_) {
            if (!out.node) continue;
            if (current_tick_id_ != 0 && out.has_symbol && out.symbol_id != current_tick_id_) continue;
            if (current_kline_id_ != 0 && out.has_symbol && out.symbol_id != current_kline_id_) continue;
            const TickRecord* tick = get_tick_for_node(out.node);
            const KlineRecord* kline = get_kline_for_node(out.node);
            if (!tick && !kline) continue;
            double val = eval_node(out.node);
            publish_signal(out, val);
        }
    }

    void publish_signal(const OutputSpec& out, double value) {
        SignalRecord sig{};
        std::memcpy(sig.source_id, out.cached_source_id, sizeof(sig.source_id));
        std::memcpy(sig.factor_name, out.cached_factor_name, sizeof(sig.factor_name));
        sig.value = value;

        const TickRecord* tick = get_tick_for_node(out.node);
        const KlineRecord* kline = get_kline_for_node(out.node);
        if (out.has_symbol && out.cached_symbol[0] != '\0') {
            std::memcpy(sig.symbol, out.cached_symbol, sizeof(sig.symbol));
            if (tick) sig.timestamp = tick->update_time;
            else if (kline) sig.timestamp = kline->start_time;
        } else {
            if (tick) {
                std::strncpy(sig.symbol, tick->symbol, sizeof(sig.symbol) - 1);
                sig.timestamp = tick->update_time;
            } else if (kline) {
                std::strncpy(sig.symbol, kline->symbol, sizeof(sig.symbol) - 1);
                sig.timestamp = kline->start_time;
            }
        }

        bus_->publish(EVENT_SIGNAL, &sig);
    }

private:
    EventBus* bus_ = nullptr;
    ITimerService* timer_svc_ = nullptr;
    FastHashMap<std::string, std::unique_ptr<FactorNodeHandle>> nodes_;
    std::vector<OutputSpec> outputs_;

    FastHashMap<uint64_t, TickRecord> last_tick_by_id_;
    FastHashMap<uint64_t, KlineRecord> last_kline_by_id_;
    uint64_t current_tick_id_ = 0;
    uint64_t current_kline_id_ = 0;

    bool trigger_on_tick_ = true;
    bool trigger_on_kline_ = false;
    bool trigger_on_timer_ = false;
    int timer_interval_sec_ = 1;
};

EXPORT_MODULE(FactorDAGModule)
