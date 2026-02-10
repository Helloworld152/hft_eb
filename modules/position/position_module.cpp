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
#include <unordered_set>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class PositionModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config, ITimerService* timer_svc = nullptr) override {
        bus_ = bus;
        timer_svc_ = timer_svc;

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

        bus_->subscribe(EVENT_RTN_TRADE, [this](void* d) {
            this->onTrade(static_cast<TradeRtn*>(d));
        });
        bus_->subscribe(EVENT_RSP_POS, [this](void* d) {
            this->onRspPos(static_cast<PositionDetail*>(d));
        });
        bus_->subscribe(EVENT_CACHE_RESET, [this](void* d) {
            this->onCacheReset(static_cast<CacheReset*>(d));
        });
    }

    void start() override {
        if (timer_svc_) {
            // 持仓查询定时器（相位 0）
            timer_svc_->add_timer(query_interval_, [this]() {
                bus_->publish(EVENT_QRY_POS, nullptr);
                if (debug_) std::cout << "[Position] [Timer] 发起持仓查询..." << std::endl;
            }, 0);
            // 资金查询定时器（相同间隔，相位偏移一半）
            timer_svc_->add_timer(query_interval_, [this]() {
                bus_->publish(EVENT_QRY_ACC, nullptr);
                if (debug_) std::cout << "[Position] [Timer] 发起资金查询..." << std::endl;
            }, 2);
            timer_svc_->add_timer(10, [this]() { dumpToJson(); });
        }
    }

    void stop() override {
        // 执行最后一次持久化
        dumpToJson();
    }

private:
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
                pos_obj["long_pnl"] = p.long_pnl;
                pos_obj["short_pnl"] = p.short_pnl;
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

private:
    bool is_shfe_ine(const char* exchange_id) {
        if (!exchange_id) return false;
        return (strcmp(exchange_id, "SHFE") == 0 || strcmp(exchange_id, "INE") == 0);
    }

    void onRspPos(PositionDetail* p) {
        std::lock_guard<std::mutex> lock(mtx_);
        
        std::string acc_id = p->account_id;
        if (acc_id.empty()) return;

        PositionDetail& local = positions_[acc_id][p->symbol_id];
        
        if (local.symbol_id == 0) {
            strncpy(local.symbol, p->symbol, 31);
            strncpy(local.account_id, p->account_id, 15);
            strncpy(local.exchange_id, p->exchange_id, 8);
            local.symbol_id = p->symbol_id;
        }

        bool is_shfe = is_shfe_ine(p->exchange_id);

        if (p->direction == '2') { // Long
            if (is_shfe) {
                // 上海合约：分阶段更新
                if (p->position_date == '1') local.long_td = p->long_td;
                else if (p->position_date == '2') local.long_yd = p->long_yd;
            } else {
                // 非上海合约：一报全覆盖
                local.long_td = p->long_td;
                local.long_yd = p->long_yd;
            }
            local.long_avg_price = p->long_avg_price;
            local.long_pnl = p->net_pnl;
        } else if (p->direction == '3') { // Short
            if (is_shfe) {
                if (p->position_date == '1') local.short_td = p->short_td;
                else if (p->position_date == '2') local.short_yd = p->short_yd;
            } else {
                local.short_td = p->short_td;
                local.short_yd = p->short_yd;
            }
            local.short_avg_price = p->short_avg_price;
            local.short_pnl = p->net_pnl;
        }
        local.net_pnl = local.long_pnl + local.short_pnl;
        
        bus_->publish(EVENT_POS_UPDATE, &local);
    }

    void onCacheReset(CacheReset* cr) {
        std::lock_guard<std::mutex> lock(mtx_);
        std::string acc_id = cr->account_id;
        
        if (cr->reset_type & 0x1) { // Position bit
            if (acc_id.empty() || acc_id[0] == '\0') {
                std::cout << "[Position] [Reset] Clearing ALL account positions. TradingDay: " 
                          << cr->trading_day << " Reason: " << cr->reason << std::endl;
                positions_.clear();
            } else {
                std::cout << "[Position] [Reset] Clearing account [" << acc_id 
                          << "] positions. TradingDay: " << cr->trading_day 
                          << " Reason: " << cr->reason << std::endl;
                positions_.erase(acc_id);
            }
        }
    }

    void onTrade(TradeRtn* rtn) {
        std::lock_guard<std::mutex> lock(mtx_);

        std::string acc_id = rtn->account_id;
        if (acc_id.empty()) acc_id = "default";

        uint64_t id = rtn->symbol_id;
        if (id == 0) id = SymbolManager::instance().get_id(rtn->symbol);

        PositionDetail& pos = positions_[acc_id][id];
        
        if (pos.symbol_id == 0) {
            pos.symbol_id = id;
            strncpy(pos.symbol, rtn->symbol, 31);
            strncpy(pos.account_id, acc_id.c_str(), 15);
            strncpy(pos.exchange_id, rtn->exchange_id, 8);
        }

        bool is_shfe = is_shfe_ine(rtn->exchange_id);

        if (rtn->offset_flag == 'O') { // 开仓
            if (rtn->direction == 'B') pos.long_td += rtn->volume;
            else pos.short_td += rtn->volume;
        } 
        else { // 平仓
            if (rtn->direction == 'S') { // 卖平 (平多头)
                if (is_shfe) {
                    if (rtn->offset_flag == 'T') pos.long_td -= rtn->volume;
                    else pos.long_yd -= rtn->volume;
                } else {
                    // 非上海：优先扣昨仓
                    if (pos.long_yd >= rtn->volume) {
                        pos.long_yd -= rtn->volume;
                    } else {
                        int remain = rtn->volume - pos.long_yd;
                        pos.long_yd = 0;
                        pos.long_td -= remain;
                    }
                }
            } else { // 买平 (平空头)
                if (is_shfe) {
                    if (rtn->offset_flag == 'T') pos.short_td -= rtn->volume;
                    else pos.short_yd -= rtn->volume;
                } else {
                    if (pos.short_yd >= rtn->volume) {
                        pos.short_yd -= rtn->volume;
                    } else {
                        int remain = rtn->volume - pos.short_yd;
                        pos.short_yd = 0;
                        pos.short_td -= remain;
                    }
                }
            }
        }
        
        // 兜底：防止逻辑异常导致负持仓
        if (pos.long_td < 0) pos.long_td = 0;
        if (pos.long_yd < 0) pos.long_yd = 0;
        if (pos.short_td < 0) pos.short_td = 0;
        if (pos.short_yd < 0) pos.short_yd = 0;

        // 立刻广播更新，确保 Monitor 同步
        bus_->publish(EVENT_POS_UPDATE, &pos);

        if (debug_) {
            std::cout << "[Position] Trade calc update: " << rtn->symbol 
                      << " Dir=" << rtn->direction << " Vol=" << rtn->volume << std::endl;
            printPosition(pos);
        }
    }

    void printPosition(const PositionDetail& pos) {
        std::cout << "[Position] [" << pos.account_id << "] Status " << pos.symbol << " | "
                  << "Long(Td/Yd): " << pos.long_td << "/" << pos.long_yd << " | "
                  << "Short(Td/Yd): " << pos.short_td << "/" << pos.short_yd 
                  << std::endl;
    }

    EventBus* bus_ = nullptr;
    ITimerService* timer_svc_ = nullptr;
    // Nested Map: AccountID -> SymbolID -> PositionDetail
    std::unordered_map<std::string, std::unordered_map<uint64_t, PositionDetail>> positions_;
    std::mutex mtx_;
    
    std::string dump_path_;
    int query_interval_ = 10;
    bool debug_ = false;
};

EXPORT_MODULE(PositionModule)
