#include "../include/trade_gateway/gateway_runtime.h"

#include <csignal>
#include <iostream>
#include <string>

namespace {

trade_gateway::GatewayRuntime* g_runtime = nullptr;

void print_usage() {
    std::cerr << "Usage: hft_trade_gateway --config <path> [--gateway-id <id>] [--account-id <account>]"
              << std::endl;
}

void handle_signal(int signum) {
    if (g_runtime) {
        std::cerr << "[TradeGateway] Caught signal " << signum << ", stopping..." << std::endl;
        g_runtime->stop();
    }
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

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    trade_gateway::GatewayRuntime runtime(std::move(config));
    g_runtime = &runtime;
    int rc = runtime.run();
    g_runtime = nullptr;
    return rc;
}
