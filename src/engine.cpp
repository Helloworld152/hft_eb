#include "../include/engine.h"
#include "../core/include/symbol_manager.h"
#include "../core/include/market_snapshot.h"
#include <dlfcn.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <array>
#include <csignal>
#include <iomanip>
#include <ctime>
#include <sstream>
#include <atomic>
#include <memory>
#include <cstdint>

#include <yaml-cpp/yaml.h>

static std::atomic<bool> g_shutdown(false);

void signal_handler(int signum) {
    std::cout << "\n[System] Caught signal " << signum << ", initiating shutdown..." << std::endl;
    g_shutdown = true;
}

// ==========================================
// Internal Implementations
// ==========================================

// --- EventBus 实现 (std::vector<std::function>) ---
class EventBusImpl : public EventBus {
public:
    void subscribe(EventType type, Handler handler) override {
        handlers_[type].push_back(std::move(handler));
    }

    void publish(EventType type, void* data) override {
        // 热路径优化：移除边界检查以提升分发性能
        for (const auto& handler : handlers_[type]) {
            handler(data);
        }
    }

    void clear() override {
        for (auto& vec : handlers_) {
            vec.clear();
        }
    }

private:
    std::array<std::vector<Handler>, MAX_EVENTS> handlers_;
};

// --- Engine 定时器适配：ITimerService 实现，转发到 Engine::add_timer_impl ---
class EngineTimerAdapter : public ITimerService {
public:
    explicit EngineTimerAdapter(HftEngine* engine) : engine_(engine) {}
    void add_timer(int interval_sec, std::function<void()> callback, int phase_sec = 0) override {
        engine_->add_timer_impl(interval_sec, std::move(callback), phase_sec);
    }
private:
    HftEngine* engine_;
};

// --- Plugin Wrapper ---
struct PluginHandle {
    void* lib_handle;
    std::shared_ptr<IModule> module;
    std::string name;

    PluginHandle() : lib_handle(nullptr), module(nullptr) {}

    ~PluginHandle() {
        // [CRITICAL] 必须先销毁模块对象，因为它的析构函数在动态库里
        module.reset();

        if (lib_handle) {
            std::cout << "[System] Unloading " << name << std::endl;
            // 实际上在复杂系统中，dlclose 可能会导致问题，有些库不建议卸载
            dlclose(lib_handle);
        }
    }
};

// ==========================================
// HftEngine Implementation
// ==========================================

HftEngine::HftEngine() : is_running_(false) {
    bus_ = std::make_unique<EventBusImpl>();
    timer_svc_ = std::make_unique<EngineTimerAdapter>(this);
}

HftEngine::~HftEngine() {
    stop();
}

bool HftEngine::loadConfig(const std::string& config_path) {
    std::cout << ">>> HFT Engine Booting using config: " << config_path << std::endl;
    
    // 显式加载核心库，并导出全局符号 (RTLD_GLOBAL)
    // 这一步至关重要，确保所有插件都能共享 Host 中的 SymbolManager 单例实例
    void* core_lib = dlopen("libhft_core.so", RTLD_GLOBAL | RTLD_NOW);
    if (!core_lib) {
        // 尝试从 bin 目录加载
        core_lib = dlopen("./bin/libhft_core.so", RTLD_GLOBAL | RTLD_NOW);
    }
    
    if (!core_lib) {
        std::cerr << "[System] Warning: Failed to load libhft_core.so globally: " << dlerror() << std::endl;
    }

    // 加载全局品种映射表
    SymbolManager::instance().load("conf/symbols.txt");

    YAML::Node config;
    try {
        config = YAML::LoadFile(config_path);
    } catch (const YAML::Exception& e) {
        std::cerr << "FATAL: YAML Parse Error: " << e.what() << std::endl;
        return false;
    }

    // [INTEGRATION] 初始化截面 (Local 或 Shm)
    if (config["snapshot"]) {
        const auto& snap = config["snapshot"];
        std::string type = snap["type"] ? snap["type"].as<std::string>() : "local";
        bool is_writer = snap["is_writer"] ? snap["is_writer"].as<bool>() : true;

        if (type == "shm") {
            std::string path = snap["path"] ? snap["path"].as<std::string>() : "/hft_snapshot";
            std::cout << "[System] Initializing SHM MarketSnapshot: " << path << (is_writer ? " (Writer)" : " (Reader)") << std::endl;
            try {
                snapshot_impl_ = std::make_unique<ShmMarketSnapshot>(path, is_writer);
            } catch (const std::exception& e) {
                std::cerr << "[System] Failed to init SHM: " << e.what() << ". Falling back to local." << std::endl;
                snapshot_impl_ = std::make_unique<LocalMarketSnapshot>();
            }
        } else {
            std::cout << "[System] Initializing Local MarketSnapshot." << std::endl;
            snapshot_impl_ = std::make_unique<LocalMarketSnapshot>();
        }
    } else {
        // 默认兜底
        std::cout << "[System] No snapshot config found, using Local MarketSnapshot." << std::endl;
        snapshot_impl_ = std::make_unique<LocalMarketSnapshot>();
    }
    
    // 设置全局单例指针
    MarketSnapshot::set_instance(snapshot_impl_.get());

    if (config["trading_hours"]) {
        const auto& th = config["trading_hours"];
        if (th["start"]) start_time_ = th["start"].as<std::string>();
        if (th["end"]) end_time_ = th["end"].as<std::string>();
        
        std::cout << "[Config] Trading Hours: " 
                  << (start_time_.empty() ? "Any" : start_time_) << " - " 
                  << (end_time_.empty() ? "Any" : end_time_) << std::endl;
    }

    if (config["plugins"] && config["plugins"].IsSequence()) {
        const auto& plugin_list = config["plugins"];
        
        for (const auto& p : plugin_list) {
            if (!p["name"] || !p["library"]) continue;

            std::string name = p["name"].as<std::string>();
            std::string lib_path = p["library"].as<std::string>();
            bool enabled = p["enabled"] ? p["enabled"].as<bool>() : true;

            if (!enabled) {
                std::cout << "[Loader] Skipping disabled module: " << name << std::endl;
                continue;
            }

            std::cout << "[Loader] Loading Module: " << name << " (" << lib_path << ")..." << std::endl;

            // A. 加载动态库
            void* handle = dlopen(lib_path.c_str(), RTLD_LAZY);
            if (!handle) {
                std::cerr << "   [ERROR] dlopen failed: " << dlerror() << std::endl;
                continue;
            }

            // B. 获取工厂
            CreateModuleFunc create_fn = (CreateModuleFunc)dlsym(handle, "create_module");
            if (!create_fn) {
                std::cerr << "   [ERROR] create_module symbol not found!" << std::endl;
                dlclose(handle);
                continue;
            }

            // C. 准备配置 map
            ConfigMap config_map;
            if (p["config"] && p["config"].IsMap()) {
                const auto& conf_node = p["config"];
                
                // 1. 注入完整 YAML 字符串，供复杂模块解析
                YAML::Emitter out;
                out << conf_node;
                config_map["_yaml"] = out.c_str();

                // 2. 扁平化简单字段
                for (YAML::const_iterator it = conf_node.begin(); it != conf_node.end(); ++it) {
                    std::string key = it->first.as<std::string>();
                    if (it->second.IsScalar()) {
                        config_map[key] = it->second.as<std::string>();
                    }
                }
            }

            // D. 实例化并初始化（传入 timer_svc 供模块注册定时任务）
            IModule* raw_ptr = create_fn();
            if (raw_ptr) {
                raw_ptr->init(bus_.get(), config_map, timer_svc_.get());

                auto plugin = std::make_shared<PluginHandle>();
                plugin->lib_handle = handle;
                plugin->module = std::shared_ptr<IModule>(raw_ptr);
                plugin->name = name;
                plugins_.push_back(plugin);
            } else {
                 std::cerr << "   [ERROR] create_module returned null!" << std::endl;
                 dlclose(handle);
            }
        }
    }
    return true;
}

void HftEngine::start() {
    if (is_running_) return;

    std::cout << ">>> All Modules Loaded. Starting..." << std::endl;
    for (auto& p : plugins_) {
        if (p->module) {
            p->module->start();
        }
    }
    is_running_ = true;
}

void HftEngine::run() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    if (!is_running_) {
        start();
    }
    
    std::cout << ">>> System Running. Waiting for signal or end time..." << std::endl;

    auto last_tick = std::chrono::steady_clock::now();

    while (!g_shutdown) {
        auto now_clock = std::chrono::steady_clock::now();
        if (now_clock - last_tick >= std::chrono::seconds(1)) {
            total_seconds_++;
            last_tick += std::chrono::seconds(1);
            run_due_timers();
        }

        // --- 结束时间检查 ---
        if (!end_time_.empty()) {
             auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
             std::tm local_tm = *std::localtime(&now);
             std::ostringstream oss;
             oss << std::put_time(&local_tm, "%H:%M:%S");
             std::string current_time = oss.str();

             if (current_time >= end_time_) {
                 std::cout << "[System] Reached end time " << end_time_ << ". Stopping." << std::endl;
                 break;
             }
        }
        
        // 降低轮询频率，减少 CPU 占用，但保证秒级精度
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    stop();
}

void HftEngine::add_timer_impl(int interval_sec, std::function<void()> cb, int phase_sec) {
    if (interval_sec <= 0) return;
    int phase = (phase_sec % interval_sec + interval_sec) % interval_sec;
    // 首次触发在「下一次 run_due_timers」之后、且 total_seconds % interval == phase 的最早时刻
    uint64_t first_run = total_seconds_ + 1;
    uint64_t base = (first_run / static_cast<uint64_t>(interval_sec)) * static_cast<uint64_t>(interval_sec);
    uint64_t next_fire = base + static_cast<uint64_t>(phase);
    if (next_fire < first_run) next_fire += static_cast<uint64_t>(interval_sec);
    timer_tasks_.push_back({ interval_sec, next_fire, std::move(cb) });
}

void HftEngine::run_due_timers() {
    for (auto& t : timer_tasks_) {
        if (total_seconds_ >= t.next_fire) {
            t.callback();
            t.next_fire += static_cast<uint64_t>(t.interval_sec);
        }
    }
}

void HftEngine::stop() {
    if (!is_running_ && plugins_.empty()) return;

    std::cout << ">>> Shutting down..." << std::endl;
    
    // 1. 停止模块
    for (auto& p : plugins_) {
        if (p && p->module) {
            p->module->stop();
        }
    }
    
    // 2. [CRITICAL] 清空所有事件回调，防止指向已卸载的内存
    if (bus_) {
        std::cout << ">>> Clearing EventBus..." << std::endl;
        bus_->clear();
    }

    // 3. [CRITICAL] 显式释放插件，确保按照预期顺序析构
    // PluginHandle 的析构函数会负责 dlclose
    plugins_.clear();
    
    is_running_ = false;
    std::cout << ">>> Shutdown Complete." << std::endl;
}
