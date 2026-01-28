#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <memory>
#include <array>

// ==========================================
// Part 1: 基础设施 (事件总线 & 工厂接口)
// ==========================================

// 1.1 极简事件总线
enum EventType { MARKET_DATA = 0, ORDER_SIGNAL, MAX_EVENTS };

class EventBus {
public:
    using Handler = std::function<void(void*)>;
    
    // 订阅：将事件ID与回调函数绑定
    void subscribe(EventType type, Handler handler) {
        handlers_[type].push_back(handler);
    }

    // 发布：直接同步调用，零拷贝
    void publish(EventType type, void* data) {
        for (auto& h : handlers_[type]) {
            h(data);
        }
    }
private:
    std::array<std::vector<Handler>, MAX_EVENTS> handlers_;
};

// 1.2 模拟 JSON 配置对象 (为了不依赖第三方库，这里用 map 模拟)
using ConfigMap = std::unordered_map<std::string, std::string>;

// 1.3 模块基类 (所有插件必须继承它)
class IModule {
public:
    virtual ~IModule() = default;
    // 核心接口：模块在这里读取配置，并把自己 "插" 到总线上
    virtual void init(EventBus* bus, const ConfigMap& config) = 0;
};

// 1.4 模块工厂 (单例)
class ModuleFactory {
public:
    using Creator = std::function<std::unique_ptr<IModule>()>;
    static ModuleFactory& instance() { static ModuleFactory f; return f; }
    
    void registerClass(const std::string& name, Creator creator) {
        creators_[name] = creator;
    }
    
    std::unique_ptr<IModule> create(const std::string& name) {
        if (creators_.count(name)) return creators_[name]();
        return nullptr;
    }
private:
    std::unordered_map<std::string, Creator> creators_;
};

// 1.5 自动注册宏 (魔法所在)
#define REGISTER_MODULE(CLASS_NAME) \
    struct Proxy##CLASS_NAME { \
        Proxy##CLASS_NAME() { \
            ModuleFactory::instance().registerClass(#CLASS_NAME, [](){ \
                return std::make_unique<CLASS_NAME>(); \
            }); \
        } \
    }; \
    static Proxy##CLASS_NAME global_proxy_##CLASS_NAME;


// ==========================================
// Part 2: 具体的业务模块 (原本应该在不同的 .cpp 文件中)
// ==========================================

// --- 事件定义 ---
struct MarketEvent { std::string symbol; double price; };
struct SignalEvent { std::string action; double price; };

// --- 模块 A: 简单策略 ---
// 逻辑：基于配置的阈值，价格高了就卖
class SimpleStrategy : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config) override {
        bus_ = bus;
        // 1. 从配置中读取参数
        threshold_ = std::stod(config.at("threshold"));
        std::string strategy_name = config.at("id");

        std::cout << "[Strategy] Loading " << strategy_name 
                  << " with threshold " << threshold_ << std::endl;

        // 2. 基于配置构建关系：订阅行情
        bus->subscribe(MARKET_DATA, [this](void* d) {
            this->onTick(static_cast<MarketEvent*>(d));
        });
    }

    void onTick(MarketEvent* e) {
        if (e->price > threshold_) {
            // 触发逻辑，发布信号
            SignalEvent sig{"SELL", e->price};
            bus_->publish(ORDER_SIGNAL, &sig);
        }
    }

private:
    double threshold_;
    EventBus* bus_;
};
REGISTER_MODULE(SimpleStrategy) // <--- 自动注册！

// --- 模块 B: 数据源 ---
// 逻辑：模拟产生数据
class MockFeed : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config) override {
        symbol_ = config.at("symbol");
        bus_ = bus;
        std::cout << "[DataFeed] Listening for " << symbol_ << std::endl;
    }

    // 模拟收到交易所推送
    void runSimulation() {
        MarketEvent e{symbol_, 105.0}; // 价格 105
        std::cout << "-> [Feed] Inbound Tick: " << e.price << std::endl;
        bus_->publish(MARKET_DATA, &e);
    }

private:
    std::string symbol_;
    EventBus* bus_;
};
REGISTER_MODULE(MockFeed) // <--- 自动注册！

// --- 模块 C: 审计日志 ---
// 逻辑：只记录下单信号
class AuditLogger : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config) override {
        std::string file = config.at("file_path");
        std::cout << "[Logger] Writing logs to " << file << std::endl;

        // 订阅信号事件
        bus->subscribe(ORDER_SIGNAL, [](void* d) {
            auto* sig = static_cast<SignalEvent*>(d);
            std::cout << "   <- [Logger] RECORDED: " << sig->action 
                      << " at " << sig->price << std::endl;
        });
    }
};
REGISTER_MODULE(AuditLogger) // <--- 自动注册！


// ==========================================
// Part 3: 主程序 (Bootloader)
// ==========================================

int main() {
    // ---------------------------------------------------------
    // 模拟读取配置文件 (config.json)
    // 注意：Main 函数完全不知道 SimpleStrategy 的类定义！
    // ---------------------------------------------------------
    struct ModuleConfig {
        std::string className;
        ConfigMap params;
    };

    std::vector<ModuleConfig> config_list = {
        // 1. 加载数据源
        {"MockFeed", {{"symbol", "BTC_USDT"}}}, 
        // 2. 加载策略 (设置阈值为 100)
        {"SimpleStrategy", {{"id", "Trend_v1"}, {"threshold", "100.0"}}},
        // 3. 加载日志 (想删掉日志功能？直接从这里注释掉即可，无需改代码)
        {"AuditLogger", {{"file_path", "/tmp/trade.log"}}}
    };

    // ---------------------------------------------------------
    // 系统启动加载 (System Loading)
    // ---------------------------------------------------------
    EventBus bus;
    std::vector<std::unique_ptr<IModule>> active_modules;
    MockFeed* feed_ptr = nullptr; // 用于稍后触发测试

    std::cout << "--- System Booting ---" << std::endl;

    for (const auto& item : config_list) {
        // A. 动态创建
        auto module = ModuleFactory::instance().create(item.className);
        
        if (module) {
            // B. 动态连线 (Wiring)
            module->init(&bus, item.params);
            
            // 为了演示方便，抓取一下 Feed 的指针
            if (item.className == "MockFeed") {
                feed_ptr = dynamic_cast<MockFeed*>(module.get());
            }

            active_modules.push_back(std::move(module));
        }
    }

    std::cout << "--- System Ready ---" << std::endl << std::endl;

    // ---------------------------------------------------------
    // 模拟运行
    // ---------------------------------------------------------
    if (feed_ptr) {
        // 产生一个 105 的价格，策略阈值是 100，应该触发 Log
        feed_ptr->runSimulation();
    }

    return 0;
}