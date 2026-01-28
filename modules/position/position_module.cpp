#include "../../include/framework.h"
#include <iostream>
#include <cstring>
#include <mutex>
#include <iomanip>

class PositionModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config) override {
        bus_ = bus;
        
        std::cout << "[Position] Initialized." << std::endl;

        // 订阅成交回报
        bus_->subscribe(EVENT_RTN_TRADE, [this](void* d) {
            this->onTrade(static_cast<TradeRtn*>(d));
        });
        
        // 订阅查询请求（可选，如果策略想主动问）
        // 目前策略主要靠监听 EVENT_POS_UPDATE 被动更新
    }

private:
    void onTrade(TradeRtn* rtn) {
        std::lock_guard<std::mutex> lock(mtx_);

        std::string symbol = rtn->symbol;
        PositionDetail& pos = positions_[symbol];
        strncpy(pos.symbol, symbol.c_str(), 31);

        // 简单的逻辑处理，暂未包含复杂的均价计算
        // Buy + Open = 多头增加
        if (rtn->direction == 'B' && rtn->offset_flag == 'O') {
            pos.long_td += rtn->volume;
            std::cout << "[Position] " << symbol << " Long Open: +" << rtn->volume << std::endl;
        }
        // Sell + Close = 多头减少
        else if (rtn->direction == 'S' && (rtn->offset_flag == 'C' || rtn->offset_flag == 'T')) {
            // 简单处理：不区分平今平昨，优先扣昨仓（逻辑简化版）
            if (rtn->offset_flag == 'T') {
                pos.long_td -= rtn->volume;
            } else {
                if (pos.long_yd >= rtn->volume) {
                    pos.long_yd -= rtn->volume;
                } else {
                    // 昨仓不够扣今仓（虽然通常交易所会明确指定，这里做个兜底）
                    int remain = rtn->volume - pos.long_yd;
                    pos.long_yd = 0;
                    pos.long_td -= remain;
                }
            }
            std::cout << "[Position] " << symbol << " Long Close: -" << rtn->volume << std::endl;
        }
        // Sell + Open = 空头增加
        else if (rtn->direction == 'S' && rtn->offset_flag == 'O') {
            pos.short_td += rtn->volume;
            std::cout << "[Position] " << symbol << " Short Open: +" << rtn->volume << std::endl;
        }
        // Buy + Close = 空头减少
        else if (rtn->direction == 'B' && (rtn->offset_flag == 'C' || rtn->offset_flag == 'T')) {
            if (rtn->offset_flag == 'T') {
                pos.short_td -= rtn->volume;
            } else {
                 if (pos.short_yd >= rtn->volume) {
                    pos.short_yd -= rtn->volume;
                } else {
                    int remain = rtn->volume - pos.short_yd;
                    pos.short_yd = 0;
                    pos.short_td -= remain;
                }
            }
            std::cout << "[Position] " << symbol << " Short Close: -" << rtn->volume << std::endl;
        }
        
        // 确保不出现负持仓（异常情况）
        if (pos.long_td < 0) pos.long_td = 0;
        if (pos.long_yd < 0) pos.long_yd = 0;
        if (pos.short_td < 0) pos.short_td = 0;
        if (pos.short_yd < 0) pos.short_yd = 0;

        // 发布持仓更新
        bus_->publish(EVENT_POS_UPDATE, &pos);
        
        printPosition(pos);
    }

    void printPosition(const PositionDetail& pos) {
        std::cout << "[Position] Status " << pos.symbol << " | "
                  << "Long(Td/Yd): " << pos.long_td << "/" << pos.long_yd << " | "
                  << "Short(Td/Yd): " << pos.short_td << "/" << pos.short_yd 
                  << std::endl;
    }

    EventBus* bus_;
    std::unordered_map<std::string, PositionDetail> positions_;
    std::mutex mtx_;
};

EXPORT_MODULE(PositionModule)
