#include "../../include/framework.h"
#include <cstring>
#include <iostream>

class StrategyModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config) override {
        bus_ = bus;
        buy_thresh_ = std::stod(config.at("buy_thresh"));
        sell_thresh_ = std::stod(config.at("sell_thresh"));
        
        std::cout << "[Strategy] Range: [" << buy_thresh_ << ", " << sell_thresh_ << "]" << std::endl;

        // 订阅行情
        bus_->subscribe(EVENT_MARKET_DATA, [this](void* d) {
            this->onTick(static_cast<TickRecord*>(d));
        });

        // 订阅持仓更新
        bus_->subscribe(EVENT_POS_UPDATE, [this](void* d) {
            this->onPosUpdate(static_cast<PositionDetail*>(d));
        });
    }

    void onTick(TickRecord* md) {
        // 防止数据还未初始化就发单
        if (md->last_price <= 0.1) return;

        // --- Buy Logic ---
        if (md->last_price < buy_thresh_) {
            // 1. 如果有空单，先平空
            int short_pos = current_pos_.short_td + current_pos_.short_yd;
            if (short_pos > 0) {
                std::cout << "[Strategy] BUY to CLOSE SHORT. Price: " << md->last_price << std::endl;
                sendOrder(md->symbol, 'B', 'C', md->last_price); // Close Short
            }
            // 2. 如果没空单，且没多单，才开多 (简化为只能持有一个方向)
            else if (current_pos_.long_td + current_pos_.long_yd == 0) {
                std::cout << "[Strategy] BUY to OPEN LONG. Price: " << md->last_price << std::endl;
                sendOrder(md->symbol, 'B', 'O', md->last_price); // Open Long
            }
        } 
        
        // --- Sell Logic ---
        else if (md->last_price > sell_thresh_) {
            // 1. 如果有多单，先平多
            int long_pos = current_pos_.long_td + current_pos_.long_yd;
            if (long_pos > 0) {
                std::cout << "[Strategy] SELL to CLOSE LONG. Price: " << md->last_price << std::endl;
                sendOrder(md->symbol, 'S', 'C', md->last_price); // Close Long
            }
            // 2. 如果没多单，且没空单，才开空
            else if (current_pos_.short_td + current_pos_.short_yd == 0) {
                std::cout << "[Strategy] SELL to OPEN SHORT. Price: " << md->last_price << std::endl;
                sendOrder(md->symbol, 'S', 'O', md->last_price); // Open Short
            }
        }
    }

    void onPosUpdate(PositionDetail* pos) {
        // 更新本地持仓缓存
        current_pos_ = *pos;
        // std::cout << "[Strategy] Pos Updated. Long: " << current_pos_.long_td + current_pos_.long_yd 
        //           << " Short: " << current_pos_.short_td + current_pos_.short_yd << std::endl;
    }

    void sendOrder(const char* symbol, char dir, char offset, double price) {
        OrderReq req;
        strncpy(req.symbol, symbol, 31);
        req.direction = dir;
        req.offset_flag = offset; // 'O'pen, 'C'lose, 'T'oday
        req.price = price;
        req.volume = 1; // 固定做 1 手
        bus_->publish(EVENT_ORDER_REQ, &req);
    }

private:
    EventBus* bus_;
    double buy_thresh_;
    double sell_thresh_;
    
    // 本地持仓缓存
    PositionDetail current_pos_ = {0}; 
};

EXPORT_MODULE(StrategyModule)