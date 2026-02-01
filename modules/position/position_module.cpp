#include "../../include/framework.h"
#include "../../core/include/symbol_manager.h"
#include <iostream>
#include <cstring>
#include <mutex>
#include <iomanip>
#include <thread>
#include <chrono>
#include <fstream>
#include <atomic>

class PositionModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config) override {
        bus_ = bus;
        
        if (config.find("dump_path") != config.end()) {
            dump_path_ = config.at("dump_path");
        } else {
            dump_path_ = "../data/pos.json";
        }

        std::cout << "[Position] Initialized. Dumping to: " << dump_path_ << std::endl;

        // 订阅成交回报
        bus_->subscribe(EVENT_RTN_TRADE, [this](void* d) {
            this->onTrade(static_cast<TradeRtn*>(d));
        });
        
        // 启动 Dump 线程
        running_ = true;
        dump_thread_ = std::thread(&PositionModule::dumpLoop, this);
    }

    void stop() override {
        running_ = false;
        if (dump_thread_.joinable()) dump_thread_.join();
    }

private:
    void dumpLoop() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            dumpToJson();
        }
    }

    void dumpToJson() {
        std::lock_guard<std::mutex> lock(mtx_);
        std::ofstream ofs(dump_path_);
        if (!ofs.is_open()) return;

        ofs << "{\n  \"positions\": [\n";
        
        bool first = true;
        for (const auto& kv : positions_) {
            const auto& p = kv.second;
            if (!first) ofs << ",\n";
            ofs << "    {"
                << "\"symbol\": \"" << p.symbol << "\", "
                << "\"long_td\": " << p.long_td << ", "
                << "\"long_yd\": " << p.long_yd << ", "
                << "\"short_td\": " << p.short_td << ", "
                << "\"short_yd\": " << p.short_yd << ", "
                << "\"net_pnl\": " << p.net_pnl
                << "}";
            first = false;
        }
        
        ofs << "\n  ],\n  \"update_time\": " << std::time(nullptr) << "\n}\n";
    }

    void onTrade(TradeRtn* rtn) {
        std::lock_guard<std::mutex> lock(mtx_);

        uint64_t id = rtn->symbol_id;
        if (id == 0) {
             // Try to recover ID if missing (should not happen if flow is correct)
             id = SymbolManager::instance().get_id(rtn->symbol);
        }

        PositionDetail& pos = positions_[id];
        
        // Initialize if new
        if (pos.symbol_id == 0) {
            pos.symbol_id = id;
            strncpy(pos.symbol, rtn->symbol, 31);
        }

        // 简单的逻辑处理，暂未包含复杂的均价计算
        // Buy + Open = 多头增加
        if (rtn->direction == 'B' && rtn->offset_flag == 'O') {
            pos.long_td += rtn->volume;
            std::cout << "[Position] " << pos.symbol << " Long Open: +" << rtn->volume << std::endl;
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
            std::cout << "[Position] " << pos.symbol << " Long Close: -" << rtn->volume << std::endl;
        }
        // Sell + Open = 空头增加
        else if (rtn->direction == 'S' && rtn->offset_flag == 'O') {
            pos.short_td += rtn->volume;
            std::cout << "[Position] " << pos.symbol << " Short Open: +" << rtn->volume << std::endl;
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
            std::cout << "[Position] " << pos.symbol << " Short Close: -" << rtn->volume << std::endl;
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
    // Key changed from string to uint64_t (hash)
    std::unordered_map<uint64_t, PositionDetail> positions_;
    std::mutex mtx_;
    
    std::string dump_path_;
    std::thread dump_thread_;
    std::atomic<bool> running_{false};
};

EXPORT_MODULE(PositionModule)
