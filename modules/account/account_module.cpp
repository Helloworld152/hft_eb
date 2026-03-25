#include "../../include/framework.h"
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct AccountState {
    AccountDetail acc{};
    uint64_t update_time_ms{0};
    uint64_t seq{0};
};

class AccountModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config, ITimerService* timer_svc = nullptr) override {
        bus_ = bus;
        timer_svc_ = timer_svc;

        if (config.find("dump_path") != config.end()) {
            dump_path_ = config.at("dump_path");
        } else {
            dump_path_ = "../data/account.json";
        }
        if (config.count("query_interval")) {
            query_interval_ = std::stoi(config.at("query_interval"));
        }
        if (config.count("debug")) debug_ = (config.at("debug") == "true");

        std::cout << "[Account] Initialized. Dumping to: " << dump_path_
                  << ", Query Interval: " << query_interval_ << "s" << std::endl;

        bus_->subscribe(EVENT_ACC_UPDATE, [this](void* d) {
            this->onAccountUpdate(static_cast<AccountDetail*>(d));
        });
        bus_->subscribe(EVENT_CACHE_RESET, [this](void* d) {
            this->onCacheReset(static_cast<CacheReset*>(d));
        });
    }

    void start() override {
        if (timer_svc_) {
            if (query_interval_ > 0) {
                timer_svc_->add_timer(query_interval_, [this]() {
                    bus_->publish(EVENT_QRY_ACC, nullptr);
                    if (debug_) std::cout << "[Account] [Timer] Query account..." << std::endl;
                }, 1);
            }
            timer_svc_->add_timer(10, [this]() { dumpToJson(); });
        }
    }

    void stop() override {
        dumpToJson();
    }

private:
    void onAccountUpdate(AccountDetail* acc) {
        if (!acc) return;
        if (acc->account_id[0] == '\0') return;

        std::lock_guard<std::mutex> lock(mtx_);
        auto& state = accounts_[std::string(acc->account_id)];
        state.acc = *acc;
        state.update_time_ms = now_ms();
        state.seq += 1;

        if (debug_) {
            std::cout << "[Account] Update: Acc=" << acc->account_id
                      << " Bal=" << acc->balance
                      << " Avail=" << acc->available
                      << " Margin=" << acc->margin << std::endl;
        }
    }

    void onCacheReset(CacheReset* cr) {
        if (!cr) return;
        if ((cr->reset_type & 0x2) == 0 && cr->reset_type != 0xFFFFFFFF) {
            return;
        }

        std::lock_guard<std::mutex> lock(mtx_);
        std::string acc_id = cr->account_id;

        if (acc_id.empty() || acc_id[0] == '\0') {
            std::cout << "[Account] [Reset] Clearing ALL accounts. TradingDay: "
                      << cr->trading_day << " Reason: " << cr->reason << std::endl;
            accounts_.clear();
        } else {
            std::cout << "[Account] [Reset] Clearing account [" << acc_id
                      << "]. TradingDay: " << cr->trading_day
                      << " Reason: " << cr->reason << std::endl;
            accounts_.erase(acc_id);
        }
    }

    void dumpToJson() {
        std::lock_guard<std::mutex> lock(mtx_);
        json root;
        root["accounts"] = json::array();
        root["timestamp"] = now_ms();

        for (const auto& kv : accounts_) {
            const auto& s = kv.second;
            json acc_obj;
            acc_obj["account_id"] = s.acc.account_id;
            acc_obj["broker_id"] = s.acc.broker_id;
            acc_obj["balance"] = s.acc.balance;
            acc_obj["available"] = s.acc.available;
            acc_obj["margin"] = s.acc.margin;
            acc_obj["close_pnl"] = s.acc.close_pnl;
            acc_obj["position_pnl"] = s.acc.position_pnl;
            acc_obj["update_time"] = s.update_time_ms;
            acc_obj["seq"] = s.seq;
            root["accounts"].push_back(acc_obj);
        }

        std::ofstream ofs(dump_path_);
        if (ofs.is_open()) {
            ofs << root.dump(2);
        }
    }

    static uint64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

private:
    EventBus* bus_ = nullptr;
    ITimerService* timer_svc_ = nullptr;
    std::unordered_map<std::string, AccountState> accounts_;
    std::mutex mtx_;

    std::string dump_path_;
    int query_interval_ = 0;
    bool debug_ = false;
};

EXPORT_MODULE(AccountModule)
