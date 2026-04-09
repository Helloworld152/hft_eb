#include "../include/trade_gateway/gateway_config.h"

#include <yaml-cpp/yaml.h>

#include <stdexcept>

namespace trade_gateway {
namespace {

std::vector<std::pair<int, int>> parse_reconnect_times(const std::string& times_str) {
    auto parse_time = [](const std::string& time_str) -> int {
        if (time_str.size() < 8) return -1;
        try {
            int h = std::stoi(time_str.substr(0, 2));
            int m = std::stoi(time_str.substr(3, 2));
            int s = std::stoi(time_str.substr(6, 2));
            return h * 10000 + m * 100 + s;
        } catch (...) {
            return -1;
        }
    };

    std::vector<std::pair<int, int>> ranges;
    size_t begin = 0;
    while (begin < times_str.size()) {
        size_t end = times_str.find(',', begin);
        std::string range = times_str.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        size_t dash = range.find('-');
        if (dash != std::string::npos) {
            int start_time = parse_time(range.substr(0, dash));
            int end_time = parse_time(range.substr(dash + 1));
            if (start_time >= 0 && end_time >= 0) {
                ranges.emplace_back(start_time, end_time);
            }
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return ranges;
}

}  // namespace

GatewayConfig load_gateway_config(const std::string& config_path) {
    YAML::Node root = YAML::LoadFile(config_path);
    GatewayConfig cfg;

    cfg.gateway_id = root["gateway_id"] ? root["gateway_id"].as<std::string>() : "";
    cfg.account_id = root["account_id"] ? root["account_id"].as<std::string>() : "";
    cfg.symbols_file = root["symbols_file"] ? root["symbols_file"].as<std::string>() : "conf/symbols.txt";
    cfg.cmd_shm = root["cmd_shm"] ? root["cmd_shm"].as<std::string>() : "";
    cfg.rtn_shm = root["rtn_shm"] ? root["rtn_shm"].as<std::string>() : "";
    cfg.ring_capacity = root["ring_capacity"] ? root["ring_capacity"].as<uint32_t>() : 1024u;
    cfg.create_rings = root["create_rings"] ? root["create_rings"].as<bool>() : true;
    cfg.unlink_on_exit = root["unlink_on_exit"] ? root["unlink_on_exit"].as<bool>() : false;
    cfg.debug = root["debug"] ? root["debug"].as<bool>() : false;

    cfg.td_front = root["td_front"] ? root["td_front"].as<std::string>() : "";
    cfg.broker_id = root["broker_id"] ? root["broker_id"].as<std::string>() : "";
    cfg.user_id = root["user_id"] ? root["user_id"].as<std::string>() : "";
    cfg.password = root["password"] ? root["password"].as<std::string>() : "";
    cfg.app_id = root["app_id"] ? root["app_id"].as<std::string>() : "";
    cfg.auth_code = root["auth_code"] ? root["auth_code"].as<std::string>() : "";
    cfg.flow_dir = root["flow_dir"] ? root["flow_dir"].as<std::string>() : "./flow_log";
    cfg.reconnect_delay_sec = root["reconnect_delay"] ? root["reconnect_delay"].as<int>() : 5;

    if (root["reconnect_times"]) {
        cfg.reconnect_time_ranges = parse_reconnect_times(root["reconnect_times"].as<std::string>());
    }

    if (cfg.account_id.empty() && !cfg.user_id.empty()) {
        cfg.account_id = cfg.user_id;
    }
    if (cfg.cmd_shm.empty() && !cfg.gateway_id.empty()) {
        cfg.cmd_shm = "/cmd_ring_" + cfg.gateway_id;
    }
    if (cfg.rtn_shm.empty() && !cfg.gateway_id.empty()) {
        cfg.rtn_shm = "/rtn_ring_" + cfg.gateway_id;
    }

    if (cfg.gateway_id.empty()) throw std::runtime_error("gateway_id is required");
    if (cfg.account_id.empty()) throw std::runtime_error("account_id is required");
    if (cfg.td_front.empty()) throw std::runtime_error("td_front is required");
    if (cfg.broker_id.empty()) throw std::runtime_error("broker_id is required");
    if (cfg.user_id.empty()) throw std::runtime_error("user_id is required");
    if (cfg.password.empty()) throw std::runtime_error("password is required");

    return cfg;
}

}  // namespace trade_gateway
