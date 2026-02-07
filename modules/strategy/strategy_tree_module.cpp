#include "../../include/framework.h"
#include <iostream>
#include <vector>
#include <memory>
#include <dlfcn.h>
#include <cstring>
#include <yaml-cpp/yaml.h>

// 策略节点句柄，管理动态库生命周期
struct StrategyNodeHandle {
    void* lib_handle;
    std::unique_ptr<IStrategyNode> node;
    std::string id;

    ~StrategyNodeHandle() {
        node.reset();
        if (lib_handle) dlclose(lib_handle);
    }
};

/**
 * StrategyTreeModule: 纯粹的二级插件容器
 * 功能：解析配置，加载叶子节点，透传所有总线事件（Tick/Kline/Signal/Rtn）
 */
class StrategyTreeModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config, ITimerService* timer_svc = nullptr) override {
        bus_ = bus;
        
        // 是否将信号同步发布到全局总线 (默认开启，供录制器使用)
        if (config.find("publish_signals") != config.end()) {
            publish_signals_ = (config.at("publish_signals") == "true");
        }

        // 兼容性检查：优先使用 _yaml
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
            std::cerr << "[策略树] YAML解析失败: " << e.what() << std::endl;
            return;
        }

        if (!doc["nodes"] || !doc["nodes"].IsSequence()) return;

        for (const auto& node_cfg : doc["nodes"]) {
            if (!node_cfg["id"] || !node_cfg["library"]) continue;

            std::string id = node_cfg["id"].as<std::string>();
            std::string lib_path = node_cfg["library"].as<std::string>();
            
            void* handle = dlopen(lib_path.c_str(), RTLD_LAZY);
            if (!handle) {
                std::cerr << "[策略树] 加载失败: " << lib_path << " | " << dlerror() << std::endl;
                continue;
            }
            
            CreateStrategyFunc create_fn = (CreateStrategyFunc)dlsym(handle, "create_strategy");
            if (!create_fn) {
                std::cerr << "[策略树] 符号未找到: create_strategy in " << lib_path << std::endl;
                dlclose(handle);
                continue;
            }

            IStrategyNode* strategy = create_fn();
            
            // 注入受限上下文
            StrategyContext* ctx = new StrategyContext(); 
            ctx->strategy_id = id;
            ctx->send_order = [this, id](const OrderReq& req) {
                bus_->publish(EVENT_ORDER_REQ, const_cast<OrderReq*>(&req));
            };
            
            // [New Design] 集中式信号分发
            ctx->send_signal = [this, id](const SignalRecord& sig) {
                SignalRecord internal_sig = sig;
                std::strncpy(internal_sig.source_id, id.c_str(), sizeof(internal_sig.source_id)-1);
                
                // 1. [Fast Path] 内部同步转发给兄弟节点
                for (auto& n : nodes_) {
                    if (n->id == id) continue; // 不发给自己，防止死循环
                    n->node->onSignal(&internal_sig);
                }

                // 2. [Slow Path] 可选发布到全局总线 (用于录制/监控)
                if (publish_signals_) {
                    bus_->publish(EVENT_SIGNAL, &internal_sig);
                }
            };

            ctx->log = [id](const char* msg) {
                std::cout << "[策略-" << id << "] " << msg << std::endl;
            };
            
            // 节点私有参数
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
                
                // [Fix] 序列化完整参数结构传给子节点，以便解析嵌套配置 (如 weights)
                YAML::Emitter out;
                out << node_cfg["params"];
                node_config["_yaml"] = out.c_str();
            }
            
            strategy->init(ctx, node_config);
            
            auto node_handle = std::make_unique<StrategyNodeHandle>();
            node_handle->lib_handle = handle;
            node_handle->node = std::unique_ptr<IStrategyNode>(strategy);
            node_handle->id = id;
            nodes_.push_back(std::move(node_handle));
        }

        // --- 事件透传 ---

        // 订阅行情 -> 分发给所有节点
        bus_->subscribe(EVENT_MARKET_DATA, [this](void* d) {
            for (auto& n : nodes_) n->node->onTick(static_cast<TickRecord*>(d));
        });

        // 订阅 K线 -> 分发
        bus_->subscribe(EVENT_KLINE, [this](void* d) {
            for (auto& n : nodes_) n->node->onKline(static_cast<KlineRecord*>(d));
        });

        // 注意：不再订阅 EVENT_SIGNAL，因为内部信号已经同步分发了
        // 如果外部有其他来源的信号，可以在这里补充，但通常策略信号都在本树内

        // 订阅成交回报 -> 分发
        bus_->subscribe(EVENT_RTN_ORDER, [this](void* d) {
            for (auto& n : nodes_) n->node->onOrderUpdate(static_cast<OrderRtn*>(d));
        });
    }

private:
    EventBus* bus_;
    std::vector<std::unique_ptr<StrategyNodeHandle>> nodes_;
    bool publish_signals_ = true;
};

EXPORT_MODULE(StrategyTreeModule)