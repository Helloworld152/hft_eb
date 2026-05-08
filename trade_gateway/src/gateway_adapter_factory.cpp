#include "../include/trade_gateway/gateway_adapter_factory.h"
#include "../include/trade_gateway/ctp_gateway_adapter.h"
#include <stdexcept>

namespace trade_gateway {

std::unique_ptr<IGatewayAdapter> create_gateway_adapter(const GatewayConfig& config) {
    if (config.adapter_type == "ctp") {
        return std::make_unique<CtpGatewayAdapter>(config);
    }
    throw std::runtime_error("unsupported adapter.type: " + config.adapter_type);
}

}  // namespace trade_gateway
