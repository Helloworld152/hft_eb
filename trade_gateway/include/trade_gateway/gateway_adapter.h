#pragma once

#include "gateway_config.h"
#include "gateway_protocol.h"

#include <functional>

namespace trade_gateway {

class IGatewayAdapter {
public:
    using EventPublisher = std::function<void(const GatewayEvent&)>;

    virtual ~IGatewayAdapter() = default;

    virtual void set_event_publisher(EventPublisher publisher) = 0;

    virtual void connect() = 0;
    virtual void stop() = 0;

    virtual bool is_ready() const noexcept = 0;
    virtual bool should_reconnect_now() const = 0;

    virtual bool submit_order(const OrderReq& req) = 0;
    virtual bool cancel_order(const CancelReq& req) = 0;
    virtual bool query_position() = 0;
    virtual bool query_account() = 0;
    virtual bool query_open_orders() = 0;
};

}  // namespace trade_gateway
