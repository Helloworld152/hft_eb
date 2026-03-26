#include "../../include/framework.h"
#include "../../core/include/order_manager.h"
#include <iostream>
#include <unordered_map>
#include <shared_mutex>
#include <cstring>

class OrderManagerModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config, ITimerService* timer_svc = nullptr) override {
        bus_ = bus;
        
        uint32_t node_id = 0;
        if (config.count("node_id")) node_id = std::stoul(config.at("node_id"));
        OrderIDGenerator::instance().set_node_id(node_id);

        if (config.count("debug")) debug_ = (config.at("debug") == "true");

        std::cout << "[OrderMgr] Hub Initialized." << std::endl;

        // 1. 拦截策略请求
        bus_->subscribe(EVENT_ORDER_REQ, [this](void* d) {
            this->handleStrategyReq(static_cast<OrderReq*>(d));
        });

        // 1.1 拦截撤单请求
        bus_->subscribe(EVENT_CANCEL_REQ, [this](void* d) {
            this->handleCancelReq(static_cast<CancelReq*>(d));
        });

        // 2. 拦截原始回报
        bus_->subscribe(EVENT_RTN_RAW_ORDER, [this](void* d) {
            this->handleRawOrder(static_cast<OrderRtn*>(d));
        });

        bus_->subscribe(EVENT_RTN_RAW_TRADE, [this](void* d) {
            this->handleRawTrade(static_cast<TradeRtn*>(d));
        });

        // 3. 拦截状态，同步 OrderRef
        bus_->subscribe(EVENT_CONN_STATUS, [this](void* d) {
            auto* cs = static_cast<ConnectionStatus*>(d);
            if (cs->status == '3' && std::string(cs->source) == "CTP_TD") {
                std::string msg(cs->msg);
                size_t pos = msg.find("MaxOrderRef:");
                if (pos != std::string::npos) {
                    uint32_t max_ref = std::stoul(msg.substr(pos + 12));
                    OrderIDGenerator::instance().set_start_ref(max_ref + 1);
                    if (debug_) {
                        std::cout << "[OrderMgr] Synced OrderRef from CTP: " << max_ref + 1 << std::endl;
                    }
                }
            }
        });
    }

private:
    void handleStrategyReq(OrderReq* req) {
        // A. 生成唯一标识
        req->client_id = OrderIDGenerator::instance().next_id();

        // B/C. 记录上下文并生成 OrderRef（必须在 publish 前释放锁：同步回调会再次进入本模块并加锁）
        {
            std::unique_lock lock(mtx_);
            auto& ctx = orders_[req->client_id];
            ctx.request = *req;
            OrderIDGenerator::instance().next_order_ref(ctx.order_ref);
            strncpy(req->order_ref, ctx.order_ref, 12);
            ref_to_id_[ctx.order_ref] = req->client_id;
        }

        if (debug_) {
            std::cout << "[OrderMgr] Decorated: CID=" << req->client_id
                      << " Ref=" << req->order_ref << " Symbol=" << req->symbol << std::endl;
        }

        bus_->publish(EVENT_ORDER_SEND, req);
    }

    void handleCancelReq(CancelReq* req) {
        CancelReq decorated{};
        {
            std::shared_lock lock(mtx_);
            auto it = orders_.find(req->client_id);
            if (it == orders_.end()) {
                std::cerr << "[OrderMgr] Warning: Cancel request for unknown CID=" << req->client_id << std::endl;
                return;
            }
            const auto& ctx = it->second;
            decorated = *req;
            strncpy(decorated.order_ref, ctx.order_ref, 12);
            strncpy(decorated.order_sys_id, ctx.order_sys_id, 20);
        }

        if (debug_) {
            std::cout << "[OrderMgr] Decorated Cancel: CID=" << req->client_id
                      << " Ref=" << decorated.order_ref << " SysID=" << decorated.order_sys_id << std::endl;
        }
        bus_->publish(EVENT_CANCEL_SEND, &decorated);
    }

    void handleRawOrder(OrderRtn* raw) {
        uint64_t cid = 0;
        {
            std::unique_lock lock(mtx_);
            auto it = ref_to_id_.find(raw->order_ref);

            if (it != ref_to_id_.end()) {
                cid = it->second;
            } else {
                cid = OrderIDGenerator::instance().next_id();
                ref_to_id_[raw->order_ref] = cid;

                auto& ctx = orders_[cid];
                ctx.request.client_id = cid;
                strncpy(ctx.request.symbol, raw->symbol, 31);
                ctx.request.symbol_id = raw->symbol_id;
                ctx.request.direction = raw->direction;
                ctx.request.offset_flag = raw->offset_flag;
                ctx.request.price = raw->limit_price;
                ctx.request.volume = raw->volume_total;
                strncpy(ctx.order_ref, raw->order_ref, 12);

                if (debug_) {
                    std::cout << "[OrderMgr] Captured External Order: CID=" << cid
                              << " Ref=" << raw->order_ref << " Symbol=" << raw->symbol << std::endl;
                }
            }

            auto& ctx = orders_[cid];
            raw->client_id = cid;
            ctx.status = raw->status;

            if (raw->order_sys_id[0] != '\0') {
                sys_to_id_[raw->order_sys_id] = cid;
                strncpy(ctx.order_sys_id, raw->order_sys_id, 20);
            }
        }

        bus_->publish(EVENT_RTN_ORDER, raw);
    }

    void handleRawTrade(TradeRtn* raw) {
        uint64_t cid = 0;
        {
            std::unique_lock lock(mtx_);
            if (raw->order_sys_id[0] != '\0') {
                auto it = sys_to_id_.find(raw->order_sys_id);
                if (it != sys_to_id_.end()) cid = it->second;
            }
            if (cid == 0) {
                auto it = ref_to_id_.find(raw->order_ref);
                if (it != ref_to_id_.end()) cid = it->second;
            }
            if (cid != 0) {
                raw->client_id = cid;
            }
        }
        if (cid != 0) {
            bus_->publish(EVENT_RTN_TRADE, raw);
        }
    }

    EventBus* bus_;
    bool debug_ = false;
    mutable std::shared_mutex mtx_;
    std::unordered_map<uint64_t, OrderContext> orders_;
    std::unordered_map<std::string, uint64_t> ref_to_id_;
    std::unordered_map<std::string, uint64_t> sys_to_id_;
};

EXPORT_MODULE(OrderManagerModule)