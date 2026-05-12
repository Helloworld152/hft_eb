#include "../include/engine.h"
#include "../core/include/core_state.h"
#include "../core/include/symbol_manager.h"
#include "../core/include/market_snapshot.h"
#include <dlfcn.h>
#include <thread>
#include <chrono>
#include <array>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <atomic>
#include <memory>
#include <cstdint>
#include <functional>  // 保留用于兼容接口
#include <immintrin.h>

#include <yaml-cpp/yaml.h>

static std::atomic<bool> g_shutdown(false);

void signal_handler(int signum) {
    (void)signum;
    g_shutdown = true;
}

// ==========================================
// Internal Implementations
// ==========================================

// --- EventBus 实现 (兼容 StaticDelegate 和 std::function) ---
class EventBusImpl : public EventBus {
public:
    // 新接口：高性能 StaticDelegate
    void subscribe(EventType type, Handler handler) override {
        sd_handlers_[type].push_back(std::move(handler));
    }

    // 兼容接口：std::function（用于过渡）
    void subscribe(EventType type, std::function<void(void*)> handler) override {
        func_handlers_[type].push_back(std::move(handler));
    }

    void publish(EventType type, void* data) override {
        // 先调用高性能 StaticDelegate handlers
        for (const auto& handler : sd_handlers_[type]) {
            handler(data);
        }
        // 再调用兼容 std::function handlers
        for (const auto& handler : func_handlers_[type]) {
            handler(data);
        }
    }

    void clear() override {
        for (auto& vec : sd_handlers_) {
            vec.clear();
        }
        for (auto& vec : func_handlers_) {
            vec.clear();
        }
    }

private:
    std::array<std::vector<Handler>, MAX_EVENTS> sd_handlers_;              // StaticDelegate handlers
    std::array<std::vector<std::function<void(void*)>>, MAX_EVENTS> func_handlers_;  // 兼容 handlers
};

// --- Engine 定时器适配：ITimerService 实现，转发到 Engine::add_timer_impl ---
class EngineTimerAdapter : public ITimerService {
public:
    explicit EngineTimerAdapter(HftEngine* engine) : engine_(engine) {}

    // 新接口：高性能 StaticDelegate
    void add_timer(int interval_sec, TimerCallback callback, int phase_sec = 0) override {
        engine_->add_timer_impl(interval_sec, std::move(callback), phase_sec);
    }

    // 兼容接口：std::function（用于过渡）
    void add_timer(int interval_sec, std::function<void()> callback, int phase_sec = 0) override {
        // 包装 std::function 为 StaticDelegate（通过 lambda 捕获，有开销）
        // 注意：这里使用堆分配来保持 lambda 存活，仅用于兼容
        auto* wrapper = new std::function<void()>(std::move(callback));
        engine_->add_timer_func(interval_sec, wrapper, phase_sec);
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
            LOG_INFO("[System] Unloading {}", name);
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
    position_service_ = std::make_unique<core::PositionService>();
    order_service_ = std::make_unique<core::OrderService>();
    account_service_ = std::make_unique<core::AccountService>();
    core::CoreServicesRegistry::set(core::CoreServices{
        position_service_.get(),
        &position_service_->store(),
        order_service_.get(),
        &order_service_->store(),
        account_service_.get(),
        &account_service_->store(),
    });
}

HftEngine::~HftEngine() {
    stop();
}

bool HftEngine::loadConfig(const std::string& config_path) {
    LOG_INFO(">>> HFT Engine Booting using config: {}", config_path);

    bus_->subscribe(EVENT_ENGINE_STOP, [](void*) {
        LOG_INFO("[System] Received EVENT_ENGINE_STOP. Stopping...");
        g_shutdown = true;
    });
    
    // 显式加载核心库，并导出全局符号 (RTLD_GLOBAL)
    // 这一步至关重要，确保所有插件都能共享 Host 中的 SymbolManager 单例实例
    void* core_lib = dlopen("libhft_core.so", RTLD_GLOBAL | RTLD_NOW);
    if (!core_lib) {
        // 尝试从 bin 目录加载
        core_lib = dlopen("./bin/libhft_core.so", RTLD_GLOBAL | RTLD_NOW);
    }
    
    if (!core_lib) {
        LOG_WARN("[System] Failed to load libhft_core.so globally: {}", dlerror());
    }

    YAML::Node config;
    try {
        config = YAML::LoadFile(config_path);
    } catch (const YAML::Exception& e) {
        LOG_ERROR("FATAL: YAML Parse Error: {}", e.what());
        return false;
    }

    // 加载全局品种映射表（可由配置覆盖）
    std::string symbols_file = "conf/symbols.txt";
    if (config["symbols_file"]) {
        symbols_file = config["symbols_file"].as<std::string>();
    }
    SymbolManager::instance().load(symbols_file);

    // [INTEGRATION] 初始化截面 (Local 或 Shm)
    if (config["snapshot"]) {
        const auto& snap = config["snapshot"];
        std::string type = snap["type"] ? snap["type"].as<std::string>() : "local";
        bool is_writer = snap["is_writer"] ? snap["is_writer"].as<bool>() : true;

        if (type == "shm") {
            std::string path = snap["path"] ? snap["path"].as<std::string>() : "/hft_snapshot";
            LOG_INFO("[System] Initializing SHM MarketSnapshot: {} ({})", path, is_writer ? "Writer" : "Reader");
            try {
                snapshot_impl_ = std::make_unique<ShmMarketSnapshot>(path, is_writer);
            } catch (const std::exception& e) {
                LOG_WARN("[System] Failed to init SHM: {}. Falling back to local.", e.what());
                snapshot_impl_ = std::make_unique<LocalMarketSnapshot>();
            }
        } else {
            LOG_INFO("[System] Initializing Local MarketSnapshot.");
            snapshot_impl_ = std::make_unique<LocalMarketSnapshot>();
        }
    } else {
        // 默认兜底
        LOG_INFO("[System] No snapshot config found, using Local MarketSnapshot.");
        snapshot_impl_ = std::make_unique<LocalMarketSnapshot>();
    }
    
    // 设置全局单例指针
    MarketSnapshot::set_instance(snapshot_impl_.get());

    if (config["trading_hours"]) {
        const auto& th = config["trading_hours"];
        if (th["start"]) start_time_ = th["start"].as<std::string>();
        if (th["end"]) {
            std::string end_str = th["end"].as<std::string>();
            int h = 0, m = 0, s = 0;
            if (std::sscanf(end_str.c_str(), "%d:%d:%d", &h, &m, &s) == 3) {
                end_time_seconds_ = h * 3600 + m * 60 + s;
            }
        }
        
        LOG_INFO("[Config] Trading Hours: {} - {}",
                 start_time_.empty() ? "Any" : start_time_,
                 end_time_seconds_ >= 0 ? std::to_string(end_time_seconds_) : "Any");
    }

    if (config["plugins"] && config["plugins"].IsSequence()) {
        const auto& plugin_list = config["plugins"];
        
        for (const auto& p : plugin_list) {
            if (!p["name"] || !p["library"]) continue;

            std::string name = p["name"].as<std::string>();
            std::string lib_path = p["library"].as<std::string>();
            bool enabled = p["enabled"] ? p["enabled"].as<bool>() : true;

            if (!enabled) {
                LOG_INFO("[Loader] Skipping disabled module: {}", name);
                continue;
            }

            LOG_INFO("[Loader] Loading Module: {} ({})...", name, lib_path);

            // A. 加载动态库
            void* handle = dlopen(lib_path.c_str(), RTLD_LOCAL | RTLD_NOW);
            if (!handle) {
                LOG_ERROR("[Loader] dlopen failed for {}: {}", lib_path, dlerror());
                continue;
            }

            // B. 获取工厂
            CreateModuleFunc create_fn = (CreateModuleFunc)dlsym(handle, "create_module");
            if (!create_fn) {
                LOG_ERROR("[Loader] create_module symbol not found: {}", lib_path);
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
                 LOG_ERROR("[Loader] create_module returned null: {}", lib_path);
                dlclose(handle);
            }
        }

    }
    return true;
}

void HftEngine::start() {
    if (is_running_) return;

    LOG_INFO(">>> All Modules Loaded. Starting...");
    position_service_->start();
    order_service_->start();
    account_service_->start();
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
    
    LOG_INFO(">>> System Running. Waiting for signal or end time...");

    auto last_tick = std::chrono::steady_clock::now();
    uint32_t idle_spins = 0;

    while (!g_shutdown) {
        // 主循环只负责发轮询驱动，具体抓取逻辑下沉到插件。
        bus_->publish(EVENT_POLL_GATEWAY, nullptr);
        bus_->publish(EVENT_POLL_REPLAY, nullptr);

        _mm_pause();
        if ((++idle_spins & 0xFFu) == 0u) {
            auto now_clock = std::chrono::steady_clock::now();
            if (now_clock - last_tick >= std::chrono::seconds(1)) {
                total_seconds_++;
                last_tick = now_clock;
                run_due_timers();

                if (end_time_seconds_ >= 0) {
                    time_t tt = std::chrono::system_clock::to_time_t(
                        std::chrono::system_clock::now());
                    struct tm local_tm;
                    localtime_r(&tt, &local_tm);
                    if (local_tm.tm_hour * 3600 + local_tm.tm_min * 60 + local_tm.tm_sec
                        >= end_time_seconds_) {
                        LOG_INFO("[System] Reached end time. Stopping.");
                        break;
                    }
                }
            }
            std::this_thread::yield();
        }
    }

    stop();
}

void HftEngine::add_timer_impl(int interval_sec, StaticDelegate<void()> cb, int phase_sec) {
    if (interval_sec <= 0) return;
    int phase = (phase_sec % interval_sec + interval_sec) % interval_sec;
    uint64_t first_run = total_seconds_ + 1;
    uint64_t base = (first_run / static_cast<uint64_t>(interval_sec)) * static_cast<uint64_t>(interval_sec);
    uint64_t next_fire = base + static_cast<uint64_t>(phase);
    if (next_fire < first_run) next_fire += static_cast<uint64_t>(interval_sec);
    TimerTask task;
    task.interval_sec = interval_sec;
    task.next_fire = next_fire;
    task.sd_callback = std::move(cb);
    task.is_static_delegate = true;
    timer_tasks_.push_back(std::move(task));
}

void HftEngine::add_timer_func(int interval_sec, std::function<void()>* cb, int phase_sec) {
    if (interval_sec <= 0) {
        delete cb;
        return;
    }
    int phase = (phase_sec % interval_sec + interval_sec) % interval_sec;
    uint64_t first_run = total_seconds_ + 1;
    uint64_t base = (first_run / static_cast<uint64_t>(interval_sec)) * static_cast<uint64_t>(interval_sec);
    uint64_t next_fire = base + static_cast<uint64_t>(phase);
    if (next_fire < first_run) next_fire += static_cast<uint64_t>(interval_sec);
    TimerTask task;
    task.interval_sec = interval_sec;
    task.next_fire = next_fire;
    task.func_callback = std::move(*cb);
    task.is_static_delegate = false;
    timer_tasks_.push_back(std::move(task));
    delete cb;
}

void HftEngine::run_due_timers() {
    for (auto& t : timer_tasks_) {
        if (total_seconds_ >= t.next_fire) {
            if (t.is_static_delegate) {
                t.sd_callback();
            } else {
                t.func_callback();
            }
            t.next_fire += static_cast<uint64_t>(t.interval_sec);
        }
    }
}

void HftEngine::stop() {
    if (!is_running_ && plugins_.empty()) return;

    LOG_INFO(">>> Shutting down...");
    
    // 1. 停止模块
    for (auto& p : plugins_) {
        if (p && p->module) {
            p->module->stop();
        }
    }

    // 2. [CRITICAL] 在 dlclose 之前销毁定时器回调：闭包代码在插件 .so 内，
    //    若先 plugins_.clear() 再析构 timer_tasks_，会段错误。
    timer_tasks_.clear();

    // 3. [CRITICAL] 清空所有事件回调，防止指向已卸载的内存
    if (bus_) {
        LOG_INFO(">>> Clearing EventBus...");
        bus_->clear();
    }

    // 4. [CRITICAL] 显式释放插件，确保按照预期顺序析构
    // PluginHandle 的析构函数会负责 dlclose
    plugins_.clear();
    account_service_->stop();
    order_service_->stop();
    position_service_->stop();
    core::CoreServicesRegistry::clear();
    
    is_running_ = false;
    LOG_INFO(">>> Shutdown Complete.");
}
