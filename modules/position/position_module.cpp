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
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class PositionModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config) override {
        bus_ = bus;
        
        if (config.find("dump_path") != config.end()) {
            dump_path_ = config.at("dump_path");
        } else {
            dump_path_ = "../data/pos.json";
        }
        
        if (config.count("query_interval")) {
            query_interval_ = std::stoi(config.at("query_interval"));
        }

        std::cout << "[Position] Initialized. Dumping to: " << dump_path_ 
                  << ", Query Interval: " << query_interval_ << "s" << std::endl;

        // 1. 订阅成交回报 (增量更新)
        bus_->subscribe(EVENT_RTN_TRADE, [this](void* d) {
            this->onTrade(static_cast<TradeRtn*>(d));
        });

        // 2. 订阅查询回报 (初始快照合并)
        bus_->subscribe(EVENT_RSP_POS, [this](void* d) {
            this->onRspPos(static_cast<PositionDetail*>(d));
        });
        
        running_ = true;
        // 启动 Dump 线程
        dump_thread_ = std::thread(&PositionModule::dumpLoop, this);
        
        // 启动主动查询线程 (如果间隔 > 0)
        if (query_interval_ > 0) {
            query_thread_ = std::thread(&PositionModule::queryLoop, this);
        }
    }

    void stop() override {
        running_ = false;
        if (dump_thread_.joinable()) dump_thread_.join();
        if (query_thread_.joinable()) query_thread_.join();
    }

private:
    void queryLoop() {
        // 启动后稍微等待一下，让 CTP 连上
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        while (running_) {
            bus_->publish(EVENT_QRY_POS, nullptr);
            // CTP 限制查询频率通常为 1次/秒，这里间隔 1.1s 确保安全
            std::this_thread::sleep_for(std::chrono::milliseconds(1100)); 
            bus_->publish(EVENT_QRY_ACC, nullptr);
            
            if (debug_) std::cout << "[Position] 发起持仓和资金查询..." << std::endl;
            
            // 等待下一个周期
            std::this_thread::sleep_for(std::chrono::seconds(query_interval_));
        }
    }

    void dumpLoop() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            dumpToJson();
        }
    }

    void dumpToJson() {
        std::lock_guard<std::mutex> lock(mtx_);
        json root;
        root["accounts"] = json::array();
        root["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        for (const auto& acc_kv : positions_) {
            json acc_obj;
            acc_obj["account_id"] = acc_kv.first;
            acc_obj["positions"] = json::array();

            for (const auto& pos_kv : acc_kv.second) {
                const auto& p = pos_kv.second;
                json pos_obj;
                pos_obj["symbol"] = p.symbol;
                pos_obj["long_td"] = p.long_td;
                pos_obj["long_yd"] = p.long_yd;
                pos_obj["short_td"] = p.short_td;
                pos_obj["short_yd"] = p.short_yd;
                pos_obj["net_pnl"] = p.net_pnl;
                acc_obj["positions"].push_back(pos_obj);
            }
            root["accounts"].push_back(acc_obj);
        }
        root["update_time"] = std::time(nullptr);

        std::ofstream ofs(dump_path_);
        if (ofs.is_open()) {
            ofs << root.dump(2); // Indent with 2 spaces
        }
    }

    void onRspPos(PositionDetail* p) {
        std::lock_guard<std::mutex> lock(mtx_);
        
        std::string acc_id = p->account_id;
        if (acc_id.empty()) return;

        PositionDetail& local = positions_[acc_id][p->symbol_id];
        
        // 初始化
        if (local.symbol_id == 0) {
            local = *p;
        } else {
            // 合并逻辑：CTP 查询回调分多空两次返回，需增量更新
            if (p->long_td > 0 || p->long_yd > 0 || p->long_avg_price > 0.0001) {
                local.long_td = p->long_td;
                local.long_yd = p->long_yd;
                local.long_avg_price = p->long_avg_price;
            }
            
            if (p->short_td > 0 || p->short_yd > 0 || p->short_avg_price > 0.0001) {
                local.short_td = p->short_td;
                local.short_yd = p->short_yd;
                local.short_avg_price = p->short_avg_price;
            }
            local.net_pnl = p->net_pnl;
        }
        
        // 广播合并后的完整状态
        bus_->publish(EVENT_POS_UPDATE, &local);
    }

    void onTrade(TradeRtn* rtn) {
        std::lock_guard<std::mutex> lock(mtx_);

        std::string acc_id = rtn->account_id;
        // 如果 account_id 为空，归类为默认账户
        if (acc_id.empty()) acc_id = "default";

        uint64_t id = rtn->symbol_id;
        if (id == 0) {
             id = SymbolManager::instance().get_id(rtn->symbol);
        }

        PositionDetail& pos = positions_[acc_id][id];
        
        // Initialize if new
        if (pos.symbol_id == 0) {
            pos.symbol_id = id;
            strncpy(pos.symbol, rtn->symbol, 31);
            strncpy(pos.account_id, acc_id.c_str(), 15);
        }

        // 简单的逻辑处理，暂未包含复杂的均价计算
        // Buy + Open = 多头增加
        if (rtn->direction == 'B' && rtn->offset_flag == 'O') {
            pos.long_td += rtn->volume;
            std::cout << "[Position] [" << acc_id << "] " << pos.symbol << " Long Open: +" << rtn->volume << std::endl;
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
            std::cout << "[Position] [" << acc_id << "] " << pos.symbol << " Long Close: -" << rtn->volume << std::endl;
        }
        // Sell + Open = 空头增加
        else if (rtn->direction == 'S' && rtn->offset_flag == 'O') {
            pos.short_td += rtn->volume;
            std::cout << "[Position] [" << acc_id << "] " << pos.symbol << " Short Open: +" << rtn->volume << std::endl;
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
            std::cout << "[Position] [" << acc_id << "] " << pos.symbol << " Short Close: -" << rtn->volume << std::endl;
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
        std::cout << "[Position] [" << pos.account_id << "] Status " << pos.symbol << " | "
                  << "Long(Td/Yd): " << pos.long_td << "/" << pos.long_yd << " | "
                  << "Short(Td/Yd): " << pos.short_td << "/" << pos.short_yd 
                  << std::endl;
    }

    EventBus* bus_;
    // Nested Map: AccountID -> SymbolID -> PositionDetail
    std::unordered_map<std::string, std::unordered_map<uint64_t, PositionDetail>> positions_;
    std::mutex mtx_;
    
    std::string dump_path_;
    std::thread dump_thread_;
    
    // Query loop members
    std::thread query_thread_;
    int query_interval_ = 10;
    bool debug_ = false;
    
    std::atomic<bool> running_{false};
};

EXPORT_MODULE(PositionModule)
