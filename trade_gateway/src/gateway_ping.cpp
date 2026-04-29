#include "../include/trade_gateway/gateway_protocol.h"
#include "../include/trade_gateway/shared_spsc_ring.h"

#include <csignal>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

volatile std::sig_atomic_t g_stop = 0;

uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

const char* command_type_name(trade_gateway::CommandType type) {
    switch (type) {
    case trade_gateway::CommandType::SubmitOrder: return "SubmitOrder";
    case trade_gateway::CommandType::CancelOrder: return "CancelOrder";
    case trade_gateway::CommandType::QueryPosition: return "QueryPosition";
    case trade_gateway::CommandType::QueryAccount: return "QueryAccount";
    case trade_gateway::CommandType::QueryOpenOrders: return "QueryOpenOrders";
    case trade_gateway::CommandType::Heartbeat: return "Heartbeat";
    case trade_gateway::CommandType::Unknown:
    default:
        return "Unknown";
    }
}

const char* event_type_name(trade_gateway::EventType type) {
    switch (type) {
    case trade_gateway::EventType::OrderRtn: return "OrderRtn";
    case trade_gateway::EventType::TradeRtn: return "TradeRtn";
    case trade_gateway::EventType::PositionRsp: return "PositionRsp";
    case trade_gateway::EventType::AccountRsp: return "AccountRsp";
    case trade_gateway::EventType::ConnectionStatus: return "ConnectionStatus";
    case trade_gateway::EventType::GatewayError: return "GatewayError";
    case trade_gateway::EventType::HeartbeatAck: return "HeartbeatAck";
    case trade_gateway::EventType::CacheReset: return "CacheReset";
    case trade_gateway::EventType::Unknown:
    default:
        return "Unknown";
    }
}

void print_usage() {
    std::cerr << "Usage: hft_trade_gateway_ping --gateway-id <id> "
                 "[--cmd-shm <name>] [--rtn-shm <name>] [--account-id <account>] "
                 "[--timeout-ms <ms>] [--max-events <n>] [--no-heartbeat]"
              << std::endl;
}

void handle_signal(int) {
    g_stop = 1;
}

void print_event(const trade_gateway::GatewayEvent& event) {
    const auto type = static_cast<trade_gateway::EventType>(event.header.type);
    std::cout << "[GatewayPing] event type=" << event_type_name(type)
              << " gateway_id=" << event.header.gateway_id
              << " account_id=" << event.header.account_id
              << " ts_ns=" << event.header.ts_ns;

    switch (type) {
    case trade_gateway::EventType::HeartbeatAck:
        break;
    case trade_gateway::EventType::ConnectionStatus:
        std::cout << " status=" << event.payload.conn.status
                  << " source=" << event.payload.conn.source
                  << " msg=" << event.payload.conn.msg;
        break;
    case trade_gateway::EventType::GatewayError:
        std::cout << " code=" << event.payload.error.code
                  << " msg=" << event.payload.error.message;
        break;
    case trade_gateway::EventType::AccountRsp:
        std::cout << " balance=" << event.payload.account.balance
                  << " available=" << event.payload.account.available
                  << " margin=" << event.payload.account.margin;
        break;
    case trade_gateway::EventType::PositionRsp:
        std::cout << " symbol=" << event.payload.position.symbol
                  << " long_td=" << event.payload.position.long_td
                  << " long_yd=" << event.payload.position.long_yd
                  << " short_td=" << event.payload.position.short_td
                  << " short_yd=" << event.payload.position.short_yd;
        break;
    case trade_gateway::EventType::OrderRtn:
        std::cout << " symbol=" << event.payload.order_rtn.symbol
                  << " order_ref=" << event.payload.order_rtn.order_ref
                  << " order_sys_id=" << event.payload.order_rtn.order_sys_id
                  << " status=" << event.payload.order_rtn.status
                  << " traded=" << event.payload.order_rtn.volume_traded
                  << "/" << event.payload.order_rtn.volume_total
                  << " msg=" << event.payload.order_rtn.status_msg;
        break;
    case trade_gateway::EventType::TradeRtn:
        std::cout << " symbol=" << event.payload.trade_rtn.symbol
                  << " order_ref=" << event.payload.trade_rtn.order_ref
                  << " trade_id=" << event.payload.trade_rtn.trade_id
                  << " price=" << event.payload.trade_rtn.price
                  << " volume=" << event.payload.trade_rtn.volume;
        break;
    case trade_gateway::EventType::CacheReset:
        std::cout << " trading_day=" << event.payload.reset.trading_day
                  << " reason=" << event.payload.reset.reason;
        break;
    case trade_gateway::EventType::Unknown:
    default:
        break;
    }
    std::cout << std::endl;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string gateway_id;
    std::string account_id;
    std::string cmd_shm;
    std::string rtn_shm;
    int timeout_ms = 3000;
    int max_events = 0;
    bool send_heartbeat = true;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--gateway-id" && i + 1 < argc) {
            gateway_id = argv[++i];
        } else if (arg == "--account-id" && i + 1 < argc) {
            account_id = argv[++i];
        } else if (arg == "--cmd-shm" && i + 1 < argc) {
            cmd_shm = argv[++i];
        } else if (arg == "--rtn-shm" && i + 1 < argc) {
            rtn_shm = argv[++i];
        } else if (arg == "--timeout-ms" && i + 1 < argc) {
            timeout_ms = std::stoi(argv[++i]);
        } else if (arg == "--max-events" && i + 1 < argc) {
            max_events = std::stoi(argv[++i]);
        } else if (arg == "--no-heartbeat") {
            send_heartbeat = false;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            std::cerr << "Unknown or incomplete argument: " << arg << std::endl;
            print_usage();
            return 1;
        }
    }

    if (gateway_id.empty()) {
        print_usage();
        return 1;
    }
    if (cmd_shm.empty()) cmd_shm = "/cmd_ring_" + gateway_id;
    if (rtn_shm.empty()) rtn_shm = "/rtn_ring_" + gateway_id;

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    trade_gateway::SharedSpscRing<trade_gateway::GatewayCommand> cmd_ring;
    trade_gateway::SharedSpscRing<trade_gateway::GatewayEvent> rtn_ring;
    try {
        cmd_ring.open(cmd_shm, 1024, false, false);
        rtn_ring.open(rtn_shm, 1024, false, false);
    } catch (const std::exception& e) {
        std::cerr << "[GatewayPing] open ring failed: " << e.what() << std::endl;
        return 2;
    }

    std::cout << "[GatewayPing] start follow mode"
              << " gateway_id=" << gateway_id
              << " account_id=" << account_id
              << " cmd_shm=" << cmd_shm
              << " rtn_shm=" << rtn_shm
              << std::endl;

    if (send_heartbeat) {
        trade_gateway::GatewayCommand cmd{};
        cmd.header.version = 1;
        cmd.header.type = static_cast<uint16_t>(trade_gateway::CommandType::Heartbeat);
        cmd.header.payload_size = sizeof(trade_gateway::EmptyPayload);
        cmd.header.ts_ns = now_ns();
        std::strncpy(cmd.header.gateway_id, gateway_id.c_str(), sizeof(cmd.header.gateway_id) - 1);
        if (!account_id.empty()) {
            std::strncpy(cmd.header.account_id, account_id.c_str(), sizeof(cmd.header.account_id) - 1);
        }

        std::cout << "[GatewayPing] send command type="
                  << command_type_name(static_cast<trade_gateway::CommandType>(cmd.header.type))
                  << " gateway_id=" << cmd.header.gateway_id
                  << " account_id=" << cmd.header.account_id
                  << std::endl;

        if (!cmd_ring.try_push(cmd)) {
            std::cerr << "[GatewayPing] push heartbeat failed: cmd ring full or not ready" << std::endl;
            return 3;
        }
    }

    int seen = 0;
    bool got_heartbeat_ack = false;
    auto last_event_at = std::chrono::steady_clock::now();
    while (!g_stop) {
        trade_gateway::GatewayEvent event{};
        if (!rtn_ring.try_pop(event)) {
            if (max_events > 0 && seen >= max_events) break;
            if (timeout_ms > 0) {
                auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - last_event_at).count();
                if (seen > 0 && idle_ms >= timeout_ms) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        ++seen;
        last_event_at = std::chrono::steady_clock::now();
        print_event(event);
        if (static_cast<trade_gateway::EventType>(event.header.type) == trade_gateway::EventType::HeartbeatAck) {
            got_heartbeat_ack = true;
        }
        if (max_events > 0 && seen >= max_events) break;
    }

    if (send_heartbeat && !got_heartbeat_ack) {
        std::cerr << "[GatewayPing] timeout without HeartbeatAck" << std::endl;
        return 4;
    }

    std::cout << "[GatewayPing] stop follow mode, events=" << seen << std::endl;
    return 0;
}
