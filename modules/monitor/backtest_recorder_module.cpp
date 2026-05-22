#include "framework.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace fs = std::filesystem;

class BacktestRecorderModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config, ITimerService* timer_svc = nullptr) override {
        (void)timer_svc;
        bus_ = bus;
        if (config.count("output_dir")) {
            output_dir_ = config.at("output_dir");
        }

        bus_->subscribe(EVENT_RTN_ORDER,
                        StaticDelegate<void(void*)>::bind<BacktestRecorderModule, &BacktestRecorderModule::on_order_event>(this));
        bus_->subscribe(EVENT_RTN_TRADE,
                        StaticDelegate<void(void*)>::bind<BacktestRecorderModule, &BacktestRecorderModule::on_trade_event>(this));
        bus_->subscribe(EVENT_ACC_UPDATE,
                        StaticDelegate<void(void*)>::bind<BacktestRecorderModule, &BacktestRecorderModule::on_account_event>(this));
    }

    void start() override {
        fs::create_directories(output_dir_);
        order_out_.open((fs::path(output_dir_) / "orders.csv").string(), std::ios::out | std::ios::trunc);
        trade_out_.open((fs::path(output_dir_) / "trades.csv").string(), std::ios::out | std::ios::trunc);
        account_out_.open((fs::path(output_dir_) / "accounts.csv").string(), std::ios::out | std::ios::trunc);

        order_out_ << "account_id,order_ref,order_sys_id,exchange_id,symbol,symbol_id,direction,offset_flag,limit_price,volume_total,volume_traded,status,status_msg\n";
        trade_out_ << "account_id,exchange_id,symbol,symbol_id,direction,offset_flag,price,volume,trade_id,order_ref,order_sys_id\n";
        account_out_ << "broker_id,account_id,balance,available,margin,close_pnl,position_pnl\n";
    }

    void stop() override {
        if (order_out_.is_open()) order_out_.close();
        if (trade_out_.is_open()) trade_out_.close();
        if (account_out_.is_open()) account_out_.close();
    }

private:
    void on_order_event(void* d) {
        const auto* rtn = static_cast<const OrderRtn*>(d);
        if (!rtn || !order_out_.is_open()) return;
        std::lock_guard<std::mutex> lock(mtx_);
        order_out_ << rtn->account_id << ','
                   << rtn->order_ref << ','
                   << rtn->order_sys_id << ','
                   << rtn->exchange_id << ','
                   << rtn->symbol << ','
                   << rtn->symbol_id << ','
                   << rtn->direction << ','
                   << rtn->offset_flag << ','
                   << rtn->limit_price << ','
                   << rtn->volume_total << ','
                   << rtn->volume_traded << ','
                   << rtn->status << ','
                   << rtn->status_msg << '\n';
        order_out_.flush();
    }

    void on_trade_event(void* d) {
        const auto* rtn = static_cast<const TradeRtn*>(d);
        if (!rtn || !trade_out_.is_open()) return;
        std::lock_guard<std::mutex> lock(mtx_);
        trade_out_ << rtn->account_id << ','
                   << rtn->exchange_id << ','
                   << rtn->symbol << ','
                   << rtn->symbol_id << ','
                   << rtn->direction << ','
                   << rtn->offset_flag << ','
                   << rtn->price << ','
                   << rtn->volume << ','
                   << rtn->trade_id << ','
                   << rtn->order_ref << ','
                   << rtn->order_sys_id << '\n';
        trade_out_.flush();
    }

    void on_account_event(void* d) {
        const auto* acc = static_cast<const AccountDetail*>(d);
        if (!acc || !account_out_.is_open()) return;
        std::lock_guard<std::mutex> lock(mtx_);
        account_out_ << acc->broker_id << ','
                     << acc->account_id << ','
                     << acc->balance << ','
                     << acc->available << ','
                     << acc->margin << ','
                     << acc->close_pnl << ','
                     << acc->position_pnl << '\n';
        account_out_.flush();
    }

    EventBus* bus_ = nullptr;
    std::string output_dir_ = "backtest_output";
    std::ofstream order_out_;
    std::ofstream trade_out_;
    std::ofstream account_out_;
    std::mutex mtx_;
};

EXPORT_MODULE(BacktestRecorderModule)
