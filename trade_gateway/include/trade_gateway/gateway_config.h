#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace trade_gateway {

struct GatewayConfig {
    std::string gateway_id;
    std::string account_id;
    std::string adapter_type;
    std::string symbols_file = "conf/symbols.txt";
    std::string cmd_shm;
    std::string rtn_shm;
    uint32_t ring_capacity = 1024;
    bool create_rings = true;
    bool unlink_on_exit = false;
    bool debug = false;

    std::string td_front;
    std::string broker_id;
    std::string user_id;
    std::string password;
    std::string app_id;
    std::string auth_code;
    std::string flow_dir = "./flow_log";
    int reconnect_delay_sec = 5;
    std::vector<std::pair<int, int>> reconnect_time_ranges;
};

GatewayConfig load_gateway_config(const std::string& config_path);

}  // namespace trade_gateway
