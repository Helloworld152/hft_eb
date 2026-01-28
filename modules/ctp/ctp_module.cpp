#include "../../include/framework.h"
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>

class CtpModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config) override {
        bus_ = bus;
        symbol_ = config.at("symbol");
        std::cout << "[CTP] Initialized for " << symbol_ << std::endl;
        
        // 订阅报单请求，模拟发单
        bus_->subscribe(EVENT_ORDER_REQ, [this](void* d) {
            auto req = static_cast<OrderReq*>(d);
            std::cout << "[CTP] -> Sending Order to Exchange: " 
                      << req->direction << " @ " << req->price << std::endl;
        });
    }

    void start() override {
        running_ = true;
        // 启动模拟行情线程
        worker_ = std::thread([this]() {
            double price = 3450.0;
            while (running_) {
                // 模拟价格波动
                price += (rand() % 100 - 45); // -45 ~ +55
                
                TickRecord md;
                memset(&md, 0, sizeof(TickRecord));
                strncpy(md.symbol, symbol_.c_str(), 31);
                md.last_price = price;
                md.volume = 1;

                std::cout << "[CTP] <- OnRtnDepthMarketData: " << price << std::endl;
                bus_->publish(EVENT_MARKET_DATA, &md);

                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        });
        worker_.detach(); // 演示用，生产环境要 join
    }

    void stop() override {
        running_ = false;
    }

private:
    EventBus* bus_;
    std::string symbol_;
    std::thread worker_;
    std::atomic<bool> running_;
};

EXPORT_MODULE(CtpModule)