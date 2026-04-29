#pragma once

#include "gateway_adapter.h"
#include "gateway_config.h"

#include <memory>

namespace trade_gateway {

std::unique_ptr<IGatewayAdapter> create_gateway_adapter(const GatewayConfig& config);

}  // namespace trade_gateway
