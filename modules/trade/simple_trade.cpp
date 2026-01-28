#include "../../include/framework.h"
#include <iostream>
#include <string>

class SimpleTradeModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config) override {
        bus_ = bus;
        
        // 读取配置中的ID，默认为 SimpleTrade
        if (config.find("id") != config.end()) {
            id_ = config.at("id");
        } else {
            id_ = "SimpleTrade";
        }

        std::cout << "[" << id_ << "] Initialized. Subscribing to EVENT_ORDER_REQ..." << std::endl;

        // 订阅报单请求事件
        bus_->subscribe(EVENT_ORDER_REQ, [this](void* d) {
            this->onOrder(static_cast<OrderReq*>(d));
        });
    }

    void onOrder(OrderReq* req) {
        std::cout << "[" << id_ << "] ORDER RECEIVED >> "
                  << "Symbol: " << req->symbol << " | "
                  << "Dir: " << req->direction << " | "
                  << "Price: " << req->price << " | "
                  << "Vol: " << req->volume 
                  << std::endl;
        
        // 模拟报单确认逻辑（此处仅打印）
        // 在真实场景中，这里会连接柜台API发送报单
    }

private:
    EventBus* bus_;
    std::string id_;
};

EXPORT_MODULE(SimpleTradeModule)
