#pragma once

#include <memory>
#include <string>

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

namespace hft::logging {

struct LoggingConfig {
    std::string app_name;
    std::string level = "info";
    std::string dir = "logs";
    std::string file_name;
    bool console = true;
    bool file = true;
    size_t max_size_mb = 100;
    size_t max_files = 10;
    std::string flush_on = "warn";
    std::string pattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] [%t] %v";
};

inline std::shared_ptr<spdlog::logger> get_logger() {
    return spdlog::default_logger();
}

void init_logging(const LoggingConfig& config);
void init_logging_from_yaml_file(const std::string& config_path, const std::string& default_app_name);
void shutdown_logging();

}  // namespace hft::logging

#define LOG_TRACE(...) do { if (auto _logger = ::hft::logging::get_logger()) _logger->trace(__VA_ARGS__); } while (0)
#define LOG_DEBUG(...) do { if (auto _logger = ::hft::logging::get_logger()) _logger->debug(__VA_ARGS__); } while (0)
#define LOG_INFO(...) do { if (auto _logger = ::hft::logging::get_logger()) _logger->info(__VA_ARGS__); } while (0)
#define LOG_WARN(...) do { if (auto _logger = ::hft::logging::get_logger()) _logger->warn(__VA_ARGS__); } while (0)
#define LOG_ERROR(...) do { if (auto _logger = ::hft::logging::get_logger()) _logger->error(__VA_ARGS__); } while (0)
#define LOG_CRITICAL(...) do { if (auto _logger = ::hft::logging::get_logger()) _logger->critical(__VA_ARGS__); } while (0)
