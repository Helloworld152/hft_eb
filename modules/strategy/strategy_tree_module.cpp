#include "../../include/framework.h"
#include <iostream>
#include <vector>
#include <memory>
#include <dlfcn.h>
#include "rapidjson/document.h"

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
    void init(EventBus* bus, const ConfigMap& config) override {
        bus_ = bus;
        
        if (config.find("_json") == config.end()) return;

        rapidjson::Document doc;
        doc.Parse(config.at("_json").c_str());
        if (!doc.IsObject() || !doc.HasMember("nodes")) return;

        for (const auto& node_cfg : doc["nodes"].GetArray()) {
            std::string id = node_cfg["id"].GetString();
            std::string lib_path = node_cfg["library"].GetString();
            
            void* handle = dlopen(lib_path.c_str(), RTLD_LAZY);
            if (!handle) {
                std::cerr << "[策略树] 加载失败: " << lib_path << " | " << dlerror() << std::endl;
                continue;
            }
            
            CreateStrategyFunc create_fn = (CreateStrategyFunc)dlsym(handle, "create_strategy");
            IStrategyNode* strategy = create_fn();
            
            // 注入受限上下文
            StrategyContext* ctx = new StrategyContext(); 
            ctx->strategy_id = id;
            ctx->send_order = [this, id](const OrderReq& req) {
                OrderReq tagged_req = req;
                bus_->publish(EVENT_ORDER_REQ, &tagged_req);
            };
            ctx->send_signal = [this, id](const SignalRecord& sig) {
                SignalRecord tagged_sig = sig;
                bus_->publish(EVENT_SIGNAL, &tagged_sig);
            };
            ctx->log = [id](const char* msg) {
                std::cout << "[策略-" << id << "] " << msg << std::endl;
            };
            
            // 节点私有参数
            ConfigMap node_config;
            if (node_cfg.HasMember("params")) {
                for (auto& m : node_cfg["params"].GetObject()) {
                    node_config[m.name.GetString()] = m.value.IsString() ? m.value.GetString() : "";
                    if (m.value.IsBool()) node_config[m.name.GetString()] = m.value.GetBool() ? "true" : "false";
                }
            }
            
            strategy->init(ctx, node_config);
            
            auto node_handle = std::make_unique<StrategyNodeHandle>();
            node_handle->lib_handle = handle;
            node_handle->node = std::unique_ptr<IStrategyNode>(strategy);
            node_handle->id = id;
            nodes_.push_back(std::move(node_handle));
        }

        // --- 事件透传：策略树不做逻辑，只做搬运工 ---

        // 订阅行情 -> 分发给所有节点
        bus_->subscribe(EVENT_MARKET_DATA, [this](void* d) {
            for (auto& n : nodes_) n->node->onTick(static_cast<TickRecord*>(d));
        });

        // 订阅 K线 -> 分发
        bus_->subscribe(EVENT_KLINE, [this](void* d) {
            for (auto& n : nodes_) n->node->onKline(static_cast<KlineRecord*>(d));
        });

        // 订阅信号 -> 分发 (用于组合节点接收信号)
        bus_->subscribe(EVENT_SIGNAL, [this](void* d) {
            for (auto& n : nodes_) n->node->onSignal(static_cast<SignalRecord*>(d));
        });

        // 订阅成交回报 -> 分发
        bus_->subscribe(EVENT_RTN_ORDER, [this](void* d) {
            for (auto& n : nodes_) n->node->onOrderUpdate(static_cast<OrderRtn*>(d));
        });
    }

private:
    EventBus* bus_;
    std::vector<std::unique_ptr<StrategyNodeHandle>> nodes_;
};

EXPORT_MODULE(StrategyTreeModule)