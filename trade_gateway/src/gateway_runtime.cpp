#include "trade_gateway/gateway_runtime.h"

#include "symbol_manager.h"
#include "logging.h"
#include "trade_gateway/gateway_adapter_factory.h"

#include <chrono>
#include <cstring>
#include <thread>

namespace trade_gateway {

GatewayRuntime::GatewayRuntime(GatewayRuntimeConfig config)
    : config_(std::move(config)) {}

GatewayRuntime::~GatewayRuntime() {
    stop();
}

bool GatewayRuntime::init() {
    gateway_config_ = load_gateway_config(config_.config_path);
    if (!config_.gateway_id.empty()) gateway_config_.gateway_id = config_.gateway_id;
    if (!config_.account_id.empty()) gateway_config_.account_id = config_.account_id;

    if (!gateway_config_.symbols_file.empty()) {
        SymbolManager::instance().load(gateway_config_.symbols_file);
    }

    cmd_ring_.open(gateway_config_.cmd_shm, gateway_config_.ring_capacity,
                   gateway_config_.create_rings, gateway_config_.unlink_on_exit);
    rtn_ring_.open(gateway_config_.rtn_shm, gateway_config_.ring_capacity,
                   gateway_config_.create_rings, gateway_config_.unlink_on_exit);

    adapter_ = create_gateway_adapter(gateway_config_);
    adapter_->set_event_publisher([this](const GatewayEvent& event) { this->publish_event(event); });
    return true;
}

int GatewayRuntime::run() {
    if (!init()) {
        return 1;
    }

    LOG_INFO("[TradeGateway] Starting gateway runtime gateway_id={} account_id={} adapter={} config={} cmd_shm={} rtn_shm={}",
             gateway_config_.gateway_id,
             gateway_config_.account_id,
             gateway_config_.adapter_type,
             config_.config_path,
             gateway_config_.cmd_shm,
             gateway_config_.rtn_shm);

    running_.store(true, std::memory_order_release);
    adapter_->connect();
    loop();
    adapter_->stop();
    return 0;
}

void GatewayRuntime::stop() {
    running_.store(false, std::memory_order_release);
}

void GatewayRuntime::loop() {
    uint64_t last_hb_ns = 0;
    uint64_t last_reconnect_attempt_ns = 0;
    uint32_t idle_spins = 0;

    while (running_.load(std::memory_order_acquire)) {
        const uint64_t now = now_ns();
        cmd_ring_.consumer_heartbeat(now);
        rtn_ring_.producer_heartbeat(now);

        GatewayCommand cmd{};
        if (cmd_ring_.try_pop(cmd)) {
            idle_spins = 0;
            handle_command(cmd);
        } else {
            detail::pause_cpu();
            if ((++idle_spins & 0xFFu) == 0u) {
                std::this_thread::yield();
            }
        }

        if (now - last_hb_ns >= 10000000000ULL) {
            last_hb_ns = now;
            publish_heartbeat_ack();
        }

        if (adapter_ && !adapter_->is_ready() && adapter_->should_reconnect_now()) {
            const uint64_t min_interval = static_cast<uint64_t>(gateway_config_.reconnect_delay_sec) * 1000000000ULL;
            if (now - last_reconnect_attempt_ns >= min_interval) {
                last_reconnect_attempt_ns = now;
                adapter_->connect();
            }
        }
    }
}

void GatewayRuntime::handle_command(const GatewayCommand& cmd) {
    const auto type = static_cast<CommandType>(cmd.header.type);
    switch (type) {
        case CommandType::SubmitOrder:
            adapter_->submit_order(cmd.payload.order_req);
            break;
        case CommandType::CancelOrder:
            adapter_->cancel_order(cmd.payload.cancel_req);
            break;
        case CommandType::QueryPosition:
            adapter_->query_position();
            break;
        case CommandType::QueryAccount:
            adapter_->query_account();
            break;
        case CommandType::QueryOpenOrders:
            adapter_->query_open_orders();
            break;
        case CommandType::Heartbeat:
            publish_heartbeat_ack();
            break;
        default:
            publish_error(GatewayErrorCode::InvalidCommand, "unknown command type");
            break;
    }
}

void GatewayRuntime::publish_event(const GatewayEvent& event) {
    if (!rtn_ring_.try_push(event)) {
        LOG_WARN("[TradeGateway] rtn ring full, dropping event type={}", event.header.type);
    }
}

void GatewayRuntime::publish_error(GatewayErrorCode code, const std::string& message) {
    GatewayEvent event{};
    event.header.version = 1;
    event.header.type = static_cast<uint16_t>(EventType::GatewayError);
    event.header.payload_size = sizeof(GatewayErrorPayload);
    event.header.ts_ns = now_ns();
    std::strncpy(event.header.gateway_id, gateway_config_.gateway_id.c_str(), sizeof(event.header.gateway_id) - 1);
    std::strncpy(event.header.account_id, gateway_config_.account_id.c_str(), sizeof(event.header.account_id) - 1);
    event.payload.error.code = static_cast<int32_t>(code);
    std::strncpy(event.payload.error.message, message.c_str(), sizeof(event.payload.error.message) - 1);
    publish_event(event);
}

void GatewayRuntime::publish_heartbeat_ack() {
    GatewayEvent event{};
    event.header.version = 1;
    event.header.type = static_cast<uint16_t>(EventType::HeartbeatAck);
    event.header.payload_size = sizeof(EmptyPayload);
    event.header.ts_ns = now_ns();
    std::strncpy(event.header.gateway_id, gateway_config_.gateway_id.c_str(), sizeof(event.header.gateway_id) - 1);
    std::strncpy(event.header.account_id, gateway_config_.account_id.c_str(), sizeof(event.header.account_id) - 1);
    publish_event(event);
}

uint64_t GatewayRuntime::now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace trade_gateway
