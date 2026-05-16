#include "logging.h"

#include <yaml-cpp/yaml.h>

#include <cstdio>
#include <filesystem>
#include <vector>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace hft::logging {
namespace {

spdlog::level::level_enum parse_level(const std::string& level_str,
                                      spdlog::level::level_enum fallback) {
    if (level_str == "trace") return spdlog::level::trace;
    if (level_str == "debug") return spdlog::level::debug;
    if (level_str == "info") return spdlog::level::info;
    if (level_str == "warn" || level_str == "warning") return spdlog::level::warn;
    if (level_str == "error" || level_str == "err") return spdlog::level::err;
    if (level_str == "critical") return spdlog::level::critical;
    if (level_str == "off") return spdlog::level::off;
    return fallback;
}

template <typename T>
T get_or_default(const YAML::Node& node, const char* key, const T& default_value) {
    try {
        if (node[key]) return node[key].as<T>();
    } catch (const YAML::Exception&) {
    }
    return default_value;
}

LoggingConfig load_config_from_yaml(const std::string& config_path, const std::string& default_app_name) {
    LoggingConfig config;
    config.app_name = default_app_name;
    config.file_name = default_app_name;

    YAML::Node root = YAML::LoadFile(config_path);
    YAML::Node logging = root["logging"];
    if (!logging) return config;

    config.level = get_or_default<std::string>(logging, "level", config.level);
    config.dir = get_or_default<std::string>(logging, "dir", config.dir);
    config.file_name = get_or_default<std::string>(logging, "file_name", config.file_name);
    config.console = get_or_default<bool>(logging, "console", config.console);
    config.file = get_or_default<bool>(logging, "file", config.file);
    config.max_size_mb = get_or_default<size_t>(logging, "max_size_mb", config.max_size_mb);
    config.max_files = get_or_default<size_t>(logging, "max_files", config.max_files);
    config.flush_on = get_or_default<std::string>(logging, "flush_on", config.flush_on);
    config.pattern = get_or_default<std::string>(logging, "pattern", config.pattern);

    if (config.file_name.empty()) {
        config.file_name = default_app_name;
    }
    if (config.max_size_mb == 0) {
        config.max_size_mb = 100;
    }
    if (config.max_files == 0) {
        config.max_files = 10;
    }
    if (!config.console && !config.file) {
        config.console = true;
    }
    return config;
}

void report_preinit_error(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
}

}  // namespace

void init_logging(const LoggingConfig& input) {
    LoggingConfig config = input;
    if (config.app_name.empty()) config.app_name = "hft_app";
    if (config.file_name.empty()) config.file_name = config.app_name;
    if (!config.console && !config.file) config.console = true;

    try {
        std::vector<spdlog::sink_ptr> sinks;
        sinks.reserve(2);

        if (config.console) {
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            sinks.push_back(console_sink);
        }

        if (config.file) {
            std::filesystem::create_directories(config.dir);
            const auto file_path = (std::filesystem::path(config.dir) / (config.file_name + ".log")).string();
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                file_path,
                config.max_size_mb * 1024 * 1024,
                config.max_files);
            sinks.push_back(file_sink);
        }

        auto logger = std::make_shared<spdlog::logger>(config.app_name, sinks.begin(), sinks.end());
        const auto level = parse_level(config.level, spdlog::level::info);
        const auto flush_level = parse_level(config.flush_on, spdlog::level::warn);
        logger->set_level(level);
        logger->flush_on(flush_level);
        logger->set_pattern(config.pattern);

        spdlog::set_default_logger(logger);
        spdlog::set_level(level);
        spdlog::flush_on(flush_level);
    } catch (const std::exception& e) {
        report_preinit_error(std::string("[Logging] init failed, fallback to default console logger: ") + e.what());
    }
}

void init_logging_from_yaml_file(const std::string& config_path, const std::string& default_app_name) {
    try {
        init_logging(load_config_from_yaml(config_path, default_app_name));
    } catch (const std::exception& e) {
        report_preinit_error(std::string("[Logging] failed to load config from ") + config_path + ": " + e.what());
        LoggingConfig fallback;
        fallback.app_name = default_app_name;
        fallback.file_name = default_app_name;
        init_logging(fallback);
    }
}

void shutdown_logging() {
    spdlog::shutdown();
}

}  // namespace hft::logging
