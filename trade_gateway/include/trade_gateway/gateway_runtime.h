#pragma once

#include "gateway_adapter.h"
#include "gateway_config.h"
#include "gateway_protocol.h"
#include "shared_spsc_ring.h"

#include <atomic>
#include <memory>
#include <string>

namespace trade_gateway {

struct GatewayRuntimeConfig {
    std::string gateway_id;
    std::string account_id;
    std::string config_path;
};

class GatewayRuntime {
public:
    explicit GatewayRuntime(GatewayRuntimeConfig config);
    ~GatewayRuntime();

    int run();
    void stop();

private:
    bool init();
    void loop();
    void handle_command(const GatewayCommand& cmd);
    void publish_event(const GatewayEvent& event);
    void publish_error(GatewayErrorCode code, const std::string& message);
    void publish_heartbeat_ack();
    static uint64_t now_ns();

    GatewayRuntimeConfig config_;
    GatewayConfig gateway_config_;
    SharedSpscRing<GatewayCommand> cmd_ring_;
    SharedSpscRing<GatewayEvent> rtn_ring_;
    std::unique_ptr<IGatewayAdapter> adapter_;
    std::atomic<bool> running_{false};
};

}  // namespace trade_gateway
