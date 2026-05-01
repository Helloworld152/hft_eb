#include "../../include/framework.h"
#include "../../core/include/core_state.h"
#include "../../core/include/order_manager.h"
#include "../../core/include/shared_spsc_ring.h"
#include "../../trade_gateway/include/trade_gateway/gateway_protocol.h"

#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>

class GatewayPollModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config, ITimerService* timer_svc = nullptr) override {
        bus_ = bus;

        if (config.count("gateway_id")) gateway_id_ = config.at("gateway_id");
        if (config.count("account_id")) account_id_ = config.at("account_id");
        if (config.count("cmd_shm")) cmd_shm_ = config.at("cmd_shm");
        if (config.count("rtn_shm")) rtn_shm_ = config.at("rtn_shm");
        if (config.count("ring_capacity")) ring_capacity_ = static_cast<uint32_t>(std::stoul(config.at("ring_capacity")));
        if (config.count("poll_batch")) poll_batch_ = static_cast<size_t>(std::stoul(config.at("poll_batch")));
        if (config.count("create_rings")) create_rings_ = is_true(config.at("create_rings"));
        if (config.count("unlink_on_exit")) unlink_on_exit_ = is_true(config.at("unlink_on_exit"));
        if (config.count("debug")) debug_ = is_true(config.at("debug"));

        if (cmd_shm_.empty() && !gateway_id_.empty()) cmd_shm_ = "/cmd_ring_" + gateway_id_;
        if (rtn_shm_.empty() && !gateway_id_.empty()) rtn_shm_ = "/rtn_ring_" + gateway_id_;

        if (gateway_id_.empty()) {
            std::cerr << "[GatewayPoll] gateway_id is required." << std::endl;
        }

        if (config.count("node_id")) {
            OrderIDGenerator::instance().set_node_id(std::stoul(config.at("node_id")));
        }

        bus_->subscribe(EVENT_POLL_GATEWAY, [this](void*) {
            this->drain_events();
        });
        bus_->subscribe(EVENT_ORDER_SEND, [this](void* d) {
            this->handle_order_send(static_cast<OrderReq*>(d));
        });
        bus_->subscribe(EVENT_CANCEL_REQ, [this](void* d) {
            this->handle_cancel_req(static_cast<CancelReq*>(d));
        });
        bus_->subscribe(EVENT_QRY_POS, [this](void*) {
            this->send_query(trade_gateway::CommandType::QueryPosition);
        });
        bus_->subscribe(EVENT_QRY_ACC, [this](void*) {
            this->send_query(trade_gateway::CommandType::QueryAccount);
        });

        std::cout << "[GatewayPoll] Initialized. gateway_id=" << gateway_id_
                  << " cmd_shm=" << cmd_shm_ << " rtn_shm=" << rtn_shm_ << std::endl;
    }

    void start() override {
        if (gateway_id_.empty()) return;

        try {
            cmd_ring_ = std::make_unique<trade_gateway::SharedSpscRing<trade_gateway::GatewayCommand>>();
            rtn_ring_ = std::make_unique<trade_gateway::SharedSpscRing<trade_gateway::GatewayEvent>>();
            cmd_ring_->open(cmd_shm_, ring_capacity_, create_rings_, unlink_on_exit_);
            rtn_ring_->open(rtn_shm_, ring_capacity_, create_rings_, unlink_on_exit_);
            running_ = true;
        } catch (const std::exception& e) {
            std::cerr << "[GatewayPoll] Failed to open shared rings: " << e.what() << std::endl;
            running_ = false;
            cmd_ring_.reset();
            rtn_ring_.reset();
        }
    }

    void stop() override {
        running_ = false;
        cmd_ring_.reset();
        rtn_ring_.reset();
    }

private:
    static bool is_true(const std::string& value) {
        return value == "1" || value == "true" || value == "TRUE" || value == "yes";
    }

    bool account_matches(const char* event_account_id) const {
        if (account_id_.empty()) return true;
        if (!event_account_id || event_account_id[0] == '\0') return true;
        return account_id_ == event_account_id;
    }

    void fill_header(trade_gateway::MessageHeader& header,
                     trade_gateway::CommandType type,
                     uint32_t payload_size,
                     const char* message_account_id = nullptr) const {
        header.version = 1;
        header.type = static_cast<uint16_t>(type);
        header.payload_size = payload_size;
        header.ts_ns = 0;
        std::strncpy(header.gateway_id, gateway_id_.c_str(), sizeof(header.gateway_id) - 1);
        const std::string& effective_account =
            (message_account_id && message_account_id[0] != '\0') ? std::string(message_account_id) : account_id_;
        if (!effective_account.empty()) {
            std::strncpy(header.account_id, effective_account.c_str(), sizeof(header.account_id) - 1);
        }
    }

    bool push_command(const trade_gateway::GatewayCommand& cmd, const char* tag) {
        if (!running_ || !cmd_ring_) return false;
        if (!cmd_ring_->try_push(cmd)) {
            std::cerr << "[GatewayPoll] cmd ring full, dropping " << tag << std::endl;
            return false;
        }
        return true;
    }

    void handle_order_send(OrderReq* req) {
        if (!req || !account_matches(req->account_id)) return;

        req->client_id = OrderIDGenerator::instance().next_id();

        auto& ctx = orders_[req->client_id];
        ctx.request = *req;
        OrderIDGenerator::instance().next_order_ref(ctx.order_ref);
        std::strncpy(req->order_ref, ctx.order_ref, sizeof(req->order_ref) - 1);
        ref_to_id_[ctx.order_ref] = req->client_id;

        trade_gateway::GatewayCommand cmd{};
        fill_header(cmd.header, trade_gateway::CommandType::SubmitOrder, sizeof(OrderReq), req->account_id);
        cmd.payload.order_req = *req;
        push_command(cmd, "SubmitOrder");
    }

    void handle_cancel_req(const CancelReq* req) {
        if (!req || !account_matches(req->account_id)) return;

        CancelReq decorated{};
        auto it = orders_.find(req->client_id);
        if (it == orders_.end()) {
            return;
        }
        const auto& ctx = it->second;
        decorated = *req;
        std::strncpy(decorated.order_ref, ctx.order_ref, sizeof(decorated.order_ref) - 1);
        std::strncpy(decorated.order_sys_id, ctx.order_sys_id, sizeof(decorated.order_sys_id) - 1);

        trade_gateway::GatewayCommand cmd{};
        fill_header(cmd.header, trade_gateway::CommandType::CancelOrder, sizeof(CancelReq), decorated.account_id);
        cmd.payload.cancel_req = decorated;
        push_command(cmd, "CancelOrder");
    }

    void send_query(trade_gateway::CommandType type) {
        trade_gateway::GatewayCommand cmd{};
        fill_header(cmd.header, type, sizeof(trade_gateway::EmptyPayload));
        push_command(cmd,
                     type == trade_gateway::CommandType::QueryPosition ? "QueryPosition" : "QueryAccount");
    }

    void drain_events() {
        if (!running_ || !rtn_ring_) return;

        for (size_t i = 0; i < poll_batch_; ++i) {
            trade_gateway::GatewayEvent event{};
            if (!rtn_ring_->try_pop(event)) {
                return;
            }
            handle_event(event);
        }
    }

    void handle_event(const trade_gateway::GatewayEvent& event) {
        const auto type = static_cast<trade_gateway::EventType>(event.header.type);
        const auto& core = core::CoreServicesRegistry::get();

        switch (type) {
        case trade_gateway::EventType::OrderRtn: {
            OrderRtn rtn = event.payload.order_rtn;
            merge_order_rtn(&rtn);
            if (core.order_service) {
                core.order_service->enqueue_order_rtn(rtn);
            }
            bus_->publish(EVENT_RTN_ORDER, &rtn);
            break;
        }
        case trade_gateway::EventType::TradeRtn: {
            TradeRtn rtn = event.payload.trade_rtn;
            if (!merge_trade_rtn(&rtn)) {
                break;
            }
            if (core.order_service) {
                core.order_service->enqueue_trade_rtn(rtn);
            }
            if (core.position_service) {
                core.position_service->enqueue_trade(rtn);
            }
            bus_->publish(EVENT_RTN_TRADE, &rtn);
            break;
        }
        case trade_gateway::EventType::PositionRsp: {
            PositionDetail pos = event.payload.position;
            if (core.position_service) {
                core.position_service->enqueue_rsp_pos(pos);
            }
            bus_->publish(EVENT_RSP_POS, &pos);
            break;
        }
        case trade_gateway::EventType::AccountRsp: {
            AccountDetail acc = event.payload.account;
            if (core.account_service) {
                core.account_service->enqueue_account(acc);
            }
            bus_->publish(EVENT_ACC_UPDATE, &acc);
            break;
        }
        case trade_gateway::EventType::ConnectionStatus: {
            ConnectionStatus status = event.payload.conn;
            sync_order_ref_if_needed(status);
            bus_->publish(EVENT_CONN_STATUS, &status);
            break;
        }
        case trade_gateway::EventType::CacheReset: {
            CacheReset reset = event.payload.reset;
            if (core.position_service) core.position_service->enqueue_reset(reset);
            if (core.order_service) core.order_service->enqueue_reset(reset);
            if (core.account_service) core.account_service->enqueue_reset(reset);
            clear_order_context(reset);
            bus_->publish(EVENT_CACHE_RESET, &reset);
            break;
        }
        case trade_gateway::EventType::GatewayError: {
            if (debug_) {
                std::cerr << "[GatewayPoll] gateway error code=" << event.payload.error.code
                          << " msg=" << event.payload.error.message << std::endl;
            }
            break;
        }
        case trade_gateway::EventType::HeartbeatAck:
        case trade_gateway::EventType::Unknown:
        default:
            break;
        }
    }

    void sync_order_ref_if_needed(const ConnectionStatus& status) {
        if (status.status != '3' || std::string(status.source) != "CTP_TD") return;
        std::string msg(status.msg);
        size_t pos = msg.find("MaxOrderRef:");
        if (pos == std::string::npos) return;
        uint32_t max_ref = std::stoul(msg.substr(pos + 12));
        OrderIDGenerator::instance().set_start_ref(max_ref + 1);
    }

    void merge_order_rtn(OrderRtn* raw) {
        uint64_t cid = 0;
        auto it = ref_to_id_.find(raw->order_ref);
        if (it != ref_to_id_.end()) {
            cid = it->second;
        } else {
            cid = OrderIDGenerator::instance().next_id();
            ref_to_id_[raw->order_ref] = cid;
        }

        auto& ctx = orders_[cid];
        if (ctx.request.client_id == 0) {
            ctx.request.client_id = cid;
            std::strncpy(ctx.request.account_id, raw->account_id, sizeof(ctx.request.account_id) - 1);
            std::strncpy(ctx.request.symbol, raw->symbol, sizeof(ctx.request.symbol) - 1);
            ctx.request.symbol_id = raw->symbol_id;
            ctx.request.direction = raw->direction;
            ctx.request.offset_flag = raw->offset_flag;
            ctx.request.price = raw->limit_price;
            ctx.request.volume = raw->volume_total;
            std::strncpy(ctx.order_ref, raw->order_ref, sizeof(ctx.order_ref) - 1);
        }

        raw->client_id = cid;
        ctx.status = raw->status;
        if (raw->order_sys_id[0] != '\0') {
            std::strncpy(ctx.order_sys_id, raw->order_sys_id, sizeof(ctx.order_sys_id) - 1);
            sys_to_id_[raw->order_sys_id] = cid;
        }
    }

    bool merge_trade_rtn(TradeRtn* raw) {
        uint64_t cid = 0;
        if (raw->order_sys_id[0] != '\0') {
            auto it = sys_to_id_.find(raw->order_sys_id);
            if (it != sys_to_id_.end()) {
                cid = it->second;
            }
        }
        if (cid == 0 && raw->order_ref[0] != '\0') {
            auto it = ref_to_id_.find(raw->order_ref);
            if (it != ref_to_id_.end()) {
                cid = it->second;
            }
        }
        if (cid == 0) return false;
        raw->client_id = cid;
        return true;
    }

    void clear_order_context(const CacheReset& reset) {
        if (reset.account_id[0] == '\0') {
            orders_.clear();
            ref_to_id_.clear();
            sys_to_id_.clear();
            return;
        }

        for (auto it = orders_.begin(); it != orders_.end();) {
            if (std::strncmp(it->second.request.account_id, reset.account_id,
                             sizeof(it->second.request.account_id)) == 0) {
                it = orders_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = ref_to_id_.begin(); it != ref_to_id_.end();) {
            if (orders_.find(it->second) == orders_.end()) it = ref_to_id_.erase(it);
            else ++it;
        }
        for (auto it = sys_to_id_.begin(); it != sys_to_id_.end();) {
            if (orders_.find(it->second) == orders_.end()) it = sys_to_id_.erase(it);
            else ++it;
        }
    }

    EventBus* bus_ = nullptr;
    std::unique_ptr<trade_gateway::SharedSpscRing<trade_gateway::GatewayCommand>> cmd_ring_;
    std::unique_ptr<trade_gateway::SharedSpscRing<trade_gateway::GatewayEvent>> rtn_ring_;
    std::string gateway_id_;
    std::string account_id_;
    std::string cmd_shm_;
    std::string rtn_shm_;
    uint32_t ring_capacity_ = 1024;
    size_t poll_batch_ = 256;
    bool create_rings_ = false;
    bool unlink_on_exit_ = false;
    bool running_ = false;
    bool debug_ = false;
    std::unordered_map<uint64_t, OrderContext> orders_;
    std::unordered_map<std::string, uint64_t> ref_to_id_;
    std::unordered_map<std::string, uint64_t> sys_to_id_;
};

EXPORT_MODULE(GatewayPollModule)
