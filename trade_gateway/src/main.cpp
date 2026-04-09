#include "../include/trade_gateway/gateway_runtime.h"

#include <iostream>
#include <string>

namespace {

void print_usage() {
    std::cerr << "Usage: hft_trade_gateway --config <path> [--gateway-id <id>] [--account-id <account>]"
              << std::endl;
}

}  // namespace

int main(int argc, char* argv[]) {
    trade_gateway::GatewayRuntimeConfig config;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--gateway-id" && i + 1 < argc) {
            config.gateway_id = argv[++i];
        } else if (arg == "--account-id" && i + 1 < argc) {
            config.account_id = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            config.config_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            std::cerr << "Unknown or incomplete argument: " << arg << std::endl;
            print_usage();
            return 1;
        }
    }

    if (config.config_path.empty()) {
        print_usage();
        return 1;
    }

    trade_gateway::GatewayRuntime runtime(std::move(config));
    return runtime.run();
}
