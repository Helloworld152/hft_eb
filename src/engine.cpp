#include "../include/engine.h"
#include <dlfcn.h>
#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <array>

#include "rapidjson/document.h"
#include "rapidjson/istreamwrapper.h"

// ==========================================
// Internal Implementations
// ==========================================

// --- EventBus 实现 ---
class EventBusImpl : public EventBus {
public:
    void subscribe(EventType type, Handler handler) override {
        if (type < 0 || type >= MAX_EVENTS) return;
        handlers_[type].push_back(handler);
    }

    void publish(EventType type, void* data) override {
        if (type < 0 || type >= MAX_EVENTS) return;
        for (auto& h : handlers_[type]) {
            h(data);
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
}

HftEngine::~HftEngine() {
    stop();
}

bool HftEngine::loadConfig(const std::string& config_path) {
    std::cout << ">>> HFT Engine Booting using config: " << config_path << std::endl;

    // 1. 读取并解析 JSON
    std::ifstream ifs(config_path);
    if (!ifs.is_open()) {
        std::cerr << "FATAL: Could not open config file!" << std::endl;
        return false;
    }
    
    rapidjson::IStreamWrapper isw(ifs);
    rapidjson::Document doc;
    doc.ParseStream(isw);

    if (doc.HasParseError()) {
        std::cerr << "FATAL: JSON Parse Error!" << std::endl;
        return false;
    }

    // 2. 遍历插件列表
    if (doc.HasMember("plugins") && doc["plugins"].IsArray()) {
        const auto& plugin_list = doc["plugins"];
        
        for (const auto& p : plugin_list.GetArray()) {
            std::string name = p["name"].GetString();
            std::string lib_path = p["library"].GetString();
            bool enabled = p.HasMember("enabled") ? p["enabled"].GetBool() : true;

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
            if (p.HasMember("config") && p["config"].IsObject()) {
                for (auto& m : p["config"].GetObject()) {
                    if (m.value.IsString()) {
                        config_map[m.name.GetString()] = m.value.GetString();
                    }
                }
            }

            // D. 实例化并初始化
            IModule* raw_ptr = create_fn();
            if (raw_ptr) {
                raw_ptr->init(bus_.get(), config_map);

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

void HftEngine::run(int duration_sec) {
    if (!is_running_) {
        start();
    }
    
    std::cout << ">>> System Running. (Simulating " << duration_sec << "s run...)" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(duration_sec));
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
