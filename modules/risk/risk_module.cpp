#include "../../include/framework.h"
#include <chrono>
#include <iostream>
#include <vector>
#include <mutex>

class RiskModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config) override {
        bus_ = bus;
        
        // 配置解析
        if (config.count("max_orders_per_second")) {
            max_orders_per_sec_ = std::stoi(config.at("max_orders_per_second"));
        }

        std::cout << "[Risk] Initialized. Max Orders/Sec: " << max_orders_per_sec_ << std::endl;

        // 订阅原始报单请求
        bus_->subscribe(EVENT_ORDER_REQ, [this](void* d) {
            this->checkRisk(static_cast<OrderReq*>(d));
        });
    }

private:
    void checkRisk(OrderReq* req) {
        std::lock_guard<std::mutex> lock(mtx_);
        
        auto now = std::chrono::steady_clock::now();
        
        // 1. 清理超过 1 秒的历史记录
        while (!order_timestamps_.empty() && 
               std::chrono::duration_cast<std::chrono::seconds>(now - order_timestamps_.front()).count() >= 1) {
            order_timestamps_.erase(order_timestamps_.begin());
        }

        // 2. 频率检查
        if (order_timestamps_.size() >= (size_t)max_orders_per_sec_) {
            std::cerr << "[Risk] REJECTED: Order rate limit exceeded! (" 
                      << max_orders_per_sec_ << " req/sec)" << std::endl;
            return;
        }

        // 3. 通过风控，记录时间戳并转发
        order_timestamps_.push_back(now);
        
        // 转发到实际发送事件
        bus_->publish(EVENT_ORDER_SEND, req);
    }

    EventBus* bus_;
    int max_orders_per_sec_ = 5;
    std::vector<std::chrono::steady_clock::time_point> order_timestamps_;
    std::mutex mtx_;
};

EXPORT_MODULE(RiskModule)
