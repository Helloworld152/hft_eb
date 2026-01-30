#include "framework.h"
#include "protocol.h"
#include <iostream>
#include <cstring>
#include <cmath>
#include <unordered_map>
#include <memory>
#include "mmap_util.h"

// 内部状态结构
struct SymbolContext {
    KlineRecord current_1m;
    bool has_1m_data = false;
    
    // 记录上一个 Tick 的累积量，用于计算增量
    int last_total_volume = 0;
    double last_total_turnover = 0.0;
    
    // 级联聚合的状态 (1H, 1D)
    KlineRecord current_1h;
    bool has_1h_data = false;

    KlineRecord current_1d;
    bool has_1d_data = false;
};

class KlineModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config) override {
        bus_ = bus;
        
        // 读取配置
        if (config.find("output_path") != config.end()) {
            output_path_ = config.at("output_path");
        } else {
            output_path_ = "../data/";
        }

        if (config.find("debug") != config.end()) {
            std::string val = config.at("debug");
            debug_ = (val == "true" || val == "1");
        }

        // 订阅原始行情 -> 生成 1M K线
        bus_->subscribe(EVENT_MARKET_DATA, [this](void* data) {
            onTick((TickRecord*)data);
        });

        // 订阅 K线事件 -> 生成 1H/1D K线 (级联)
        bus_->subscribe(EVENT_KLINE, [this](void* data) {
            onKline((KlineRecord*)data);
        });

        std::cout << "[KlineModule] Initialized. Output: " << output_path_ 
                  << " Debug: " << (debug_ ? "ON" : "OFF") << std::endl;
    }

private:
    EventBus* bus_ = nullptr;
    std::string output_path_;
    bool debug_ = false;
    std::unordered_map<std::string, SymbolContext> contexts_;
    
    // Writers
    std::unique_ptr<MmapWriter<KlineRecord>> writer_1m_;
    std::unique_ptr<MmapWriter<KlineRecord>> writer_1h_;
    std::unique_ptr<MmapWriter<KlineRecord>> writer_1d_;
    uint32_t current_writer_day_ = 0;

    // 辅助：获取对齐后的时间 (HHMMSSmmm)
    // 简单实现：将 HHMMSSmmm 转换为分钟对齐
    // 注意：这里假设 update_time 是 HHMMSSmmm 格式的整数
    // 例如 093005500 -> 93005500
    // 为了简化，我们先处理 HHMMSS 部分
    uint64_t align_to_minute(uint64_t time_hhmmssmmm) {
        int mmm = time_hhmmssmmm % 1000;
        uint64_t time_sec = time_hhmmssmmm / 1000;
        int s = time_sec % 100;
        int m = (time_sec / 100) % 100;
        int h = time_sec / 10000;
        
        // 对齐到分钟开始: 秒和毫秒置零
        return (h * 10000 + m * 100) * 1000; 
    }

    void onTick(const TickRecord* tick) {
        std::string symbol = tick->symbol;
        SymbolContext& ctx = contexts_[symbol];

        uint64_t tick_time = tick->update_time;
        uint64_t aligned_time = align_to_minute(tick_time);

        // 初始化
        if (!ctx.has_1m_data) {
            init_kline(ctx.current_1m, tick, K_1M, aligned_time);
            ctx.last_total_volume = tick->volume;
            ctx.last_total_turnover = tick->turnover;
            ctx.has_1m_data = true;
            return;
        }

        // 检查是否跨周期 (Next-Tick Trigger)
        // 如果当前 Tick 的对齐时间 > 当前 Bar 的开始时间，说明新的一分钟开始了
        if (aligned_time > ctx.current_1m.start_time || tick->trading_day > ctx.current_1m.trading_day) {
            // 闭合当前 Bar
            publish_kline(ctx.current_1m);
            
            // 开启新 Bar
            init_kline(ctx.current_1m, tick, K_1M, aligned_time);
        }

        // 无论是新 Bar 还是旧 Bar，都需要用当前 Tick 更新状态
        // 对于新 Bar，volume = tick->volume - ctx.last_total_volume (即当前 Tick 的增量)
        update_kline(ctx.current_1m, tick, ctx.last_total_volume, ctx.last_total_turnover);
        
        // 更新累积量缓存
        ctx.last_total_volume = tick->volume;
        ctx.last_total_turnover = tick->turnover;
    }

    void init_kline(KlineRecord& k, const TickRecord* tick, KlineInterval interval, uint64_t start_time) {
        strncpy(k.symbol, tick->symbol, sizeof(k.symbol));
        k.trading_day = tick->trading_day;
        k.start_time = start_time;
        k.open = tick->last_price;
        k.high = tick->last_price;
        k.low = tick->last_price;
        k.close = tick->last_price;
        k.interval = interval;
        k.open_interest = tick->open_interest;
        
        // 对于新 Bar，如果是由 onTick 触发的初始化，
        // 它的初始 Volume 应该是 (Tick.Volume - Previous_Tick.Volume)
        // 但在 init_kline 被调用时，我们通常认为它是 Bar 的第一笔
        // 这里的 Volume/Turnover 在 update 时会累加，或者重新计算
        // 修正逻辑：init 时 volume/turnover 设为 0，然后立刻 update 一次？
        // 或者：init 时 volume = tick->volume - ctx.last_total_volume
        // 由于 init_kline 参数里没有 ctx，我们在外面处理比较好。
        // 这里暂时设为 0，调用完 init 后，调用者应该负责计算第一笔的增量
        k.volume = 0;
        k.turnover = 0;
    }

    void update_kline(KlineRecord& k, const TickRecord* tick, int base_volume, double base_turnover) {
        if (tick->last_price > k.high) k.high = tick->last_price;
        if (tick->last_price < k.low) k.low = tick->last_price;
        k.close = tick->last_price;
        k.open_interest = tick->open_interest;
        
        // 计算增量
        k.volume = tick->volume - base_volume;
        k.turnover = tick->turnover - base_turnover;
    }

    void check_writer(uint32_t trading_day) {
        if (current_writer_day_ == trading_day && writer_1m_) return;
        
        current_writer_day_ = trading_day;
        std::string day_str = std::to_string(trading_day);
        
        try {
            // 预分配容量：1M (100万), 1H (5万), 1D (5000)
            writer_1m_ = std::make_unique<MmapWriter<KlineRecord>>(
                output_path_ + "/kline_1m_" + day_str, 2000000); // 2M slots enough for all symbols
            
            writer_1h_ = std::make_unique<MmapWriter<KlineRecord>>(
                output_path_ + "/kline_1h_" + day_str, 100000);
            
            writer_1d_ = std::make_unique<MmapWriter<KlineRecord>>(
                output_path_ + "/kline_1d_" + day_str, 10000);
                
            std::cout << "[KlineModule] Writers created for day " << day_str << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[KlineModule] Failed to create writers: " << e.what() << std::endl;
        }
    }

    void publish_kline(KlineRecord& k) {
        if (debug_) {
            std::cout << "[KlineModule][DEBUG] Publish Kline: " 
                      << k.symbol << " " 
                      << k.start_time << " "
                      << "O:" << k.open << " C:" << k.close 
                      << " V:" << k.volume << std::endl;
        }

        // 分发事件
        bus_->publish(EVENT_KLINE, &k);
        
        // 持久化
        check_writer(k.trading_day);
        
        if (k.interval == K_1M && writer_1m_) writer_1m_->write(k);
        else if (k.interval == K_1H && writer_1h_) writer_1h_->write(k);
        else if (k.interval == K_1D && writer_1d_) writer_1d_->write(k);
    }

    void onKline(const KlineRecord* kline) {
        // 级联合成逻辑
        if (kline->interval == K_1M) {
            // 合成 1H
            process_cascade(kline, K_1H);
        } else if (kline->interval == K_1H) {
            // 合成 1D
            process_cascade(kline, K_1D);
        }
    }

    void process_cascade(const KlineRecord* input, KlineInterval target_interval) {
        std::string symbol = input->symbol;
        SymbolContext& ctx = contexts_[symbol];
        
        KlineRecord* target = nullptr;
        bool* has_data = nullptr;
        
        if (target_interval == K_1H) {
            target = &ctx.current_1h;
            has_data = &ctx.has_1h_data;
        } else if (target_interval == K_1D) {
            target = &ctx.current_1d;
            has_data = &ctx.has_1d_data;
        } else {
            return;
        }

        // 计算输入 Bar 所属的目标周期起始时间
        uint64_t input_aligned_start = 0;
        if (target_interval == K_1H) {
             // HHMMSSmmm -> HH0000000
             // 需要去掉后 7 位 (MMSSmmm)
             input_aligned_start = (input->start_time / 10000000) * 10000000; 
        } else {
             // 日线起始时间记为 0 (或由外部定义)
             input_aligned_start = 0; 
        }

        if (!(*has_data)) {
            *target = *input; // 复制基础信息
            target->interval = target_interval;
            target->start_time = input_aligned_start;
            target->volume = input->volume;
            target->turnover = input->turnover;
            *has_data = true;
        } else {
             // 检查是否闭合 (跨周期)
             bool closed = false;
             
             if (target_interval == K_1H) {
                 // 如果新进来的 Bar 属于下一个小时 (或更晚)
                 if (input_aligned_start > target->start_time) { 
                     closed = true;
                 }
             } else if (target_interval == K_1D) {
                 if (input->trading_day > target->trading_day) {
                     closed = true;
                 }
             }

             if (closed) {
                 publish_kline(*target);
                 // Reset using the new input as the first bar of the new cycle
                 *target = *input;
                 target->interval = target_interval;
                 target->start_time = input_aligned_start;
                 target->volume = input->volume;
                 target->turnover = input->turnover;
             } else {
                 // Update aggregation
                 if (input->high > target->high) target->high = input->high;
                 if (input->low < target->low) target->low = input->low;
                 target->close = input->close; // Close is always the latest
                 target->open_interest = input->open_interest;
                 target->volume += input->volume;
                 target->turnover += input->turnover;
             }
        }
    }
};

EXPORT_MODULE(KlineModule)
