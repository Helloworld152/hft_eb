#pragma once

#include <cstdint>
#include <type_traits>

#include "protocol.h"

namespace trade_gateway {

enum class CommandType : uint16_t {
    Unknown = 0,
    SubmitOrder = 1,
    CancelOrder = 2,
    QueryPosition = 3,
    QueryAccount = 4,
    QueryOpenOrders = 5,
    Heartbeat = 6,
};

enum class EventType : uint16_t {
    Unknown = 0,
    OrderRtn = 1,
    TradeRtn = 2,
    PositionRsp = 3,
    AccountRsp = 4,
    ConnectionStatus = 5,
    GatewayError = 6,
    HeartbeatAck = 7,
    CacheReset = 8,
};

enum class GatewayErrorCode : int32_t {
    None = 0,
    NotReady = 1,
    QueueFull = 2,
    InvalidCommand = 3,
    TransportError = 4,
    ApiError = 5,
    ConfigError = 6,
};

struct MessageHeader {
    uint16_t version = 1;
    uint16_t type = 0;
    uint32_t payload_size = 0;
    uint64_t ts_ns = 0;
    char gateway_id[32] = {};
    char account_id[16] = {};
};

struct EmptyPayload {
    char reserved = 0;
};

struct GatewayErrorPayload {
    int32_t code = 0;
    char message[128] = {};
};

union CommandPayload {
    EmptyPayload empty;
    OrderReq order_req;
    CancelReq cancel_req;
};

union EventPayload {
    EmptyPayload empty;
    OrderRtn order_rtn;
    TradeRtn trade_rtn;
    PositionDetail position;
    AccountDetail account;
    ConnectionStatus conn;
    CacheReset reset;
    GatewayErrorPayload error;
};

struct GatewayCommand {
    MessageHeader header{};
    CommandPayload payload{};
};

struct GatewayEvent {
    MessageHeader header{};
    EventPayload payload{};
};

static_assert(std::is_trivially_copyable<OrderReq>::value, "OrderReq must be trivially copyable");
static_assert(std::is_trivially_copyable<CancelReq>::value, "CancelReq must be trivially copyable");
static_assert(std::is_trivially_copyable<OrderRtn>::value, "OrderRtn must be trivially copyable");
static_assert(std::is_trivially_copyable<TradeRtn>::value, "TradeRtn must be trivially copyable");
static_assert(std::is_trivially_copyable<PositionDetail>::value, "PositionDetail must be trivially copyable");
static_assert(std::is_trivially_copyable<AccountDetail>::value, "AccountDetail must be trivially copyable");
static_assert(std::is_trivially_copyable<ConnectionStatus>::value, "ConnectionStatus must be trivially copyable");
static_assert(std::is_trivially_copyable<CacheReset>::value, "CacheReset must be trivially copyable");
static_assert(std::is_trivially_copyable<GatewayCommand>::value, "GatewayCommand must be trivially copyable");
static_assert(std::is_trivially_copyable<GatewayEvent>::value, "GatewayEvent must be trivially copyable");

}  // namespace trade_gateway
