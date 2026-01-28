#include "protocol.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <unordered_map>
#include <iomanip>
#include <cmath>
#include <cstring>

struct Bar {
    char symbol[32];
    uint64_t start_time; // HHMM00000 (ms)
    double open;
    double high;
    double low;
    double close;
    int volume;          // 周期内成交量
    double turnover;     // 周期内成交额
    
    // 辅助状态，用于差分计算
    int last_tick_vol;
    double last_tick_turnover;
    bool initialized;
};

// 简单的 K 线生成器
class BarGenerator {
public:
    BarGenerator(int interval_min) : interval_ms_(interval_min * 60 * 1000) {}

    void process_tick(const TickRecord& tick) {
        std::string symbol = tick.symbol;
        Bar& bar = context_[symbol];

        // 提取时间 (HHMMSSmmm -> ms from 00:00:00)
        uint64_t t = tick.update_time;
        int hh = t / 10000000;
        int mm = (t / 100000) % 100;
        int ss = (t / 1000) % 100;
        int ms = t % 1000;
        
        long current_ms = hh * 3600000 + mm * 60000 + ss * 1000 + ms;
        long bar_start_ms = (current_ms / interval_ms_) * interval_ms_;

        // 初始化
        if (!bar.initialized) {
            init_bar(bar, tick, bar_start_ms);
            return; // 第一个 Tick 仅用于初始化状态，不计入成交量（除非是当天的第一个 Tick，这里简化处理）
        }

        // 检查新周期
        if (current_ms >= bar.start_time + interval_ms_) {
            finish_bar(bar);
            init_bar(bar, tick, bar_start_ms);
            // 这里新 Bar 的 Volume 需要包含当前 Tick 的增量
            // 但因为 init_bar 会重置 volume=0，我们需要修正
        }
        
        // 计算增量
        int vol_delta = tick.volume - bar.last_tick_vol;
        double to_delta = tick.turnover - bar.last_tick_turnover;

        // 过滤非法数据（CTP 有时会推重复或者乱序）
        if (vol_delta < 0) vol_delta = 0; 
        if (to_delta < 0) to_delta = 0;

        // 更新 K 线
        bar.high = std::max(bar.high, tick.last_price);
        bar.low = std::min(bar.low, tick.last_price);
        bar.close = tick.last_price;
        bar.volume += vol_delta;
        bar.turnover += to_delta;

        // 更新状态
        bar.last_tick_vol = tick.volume;
        bar.last_tick_turnover = tick.turnover;
    }

    void finish_all() {
        for (auto& pair : context_) {
            if (pair.second.initialized) {
                finish_bar(pair.second);
            }
        }
    }

private:
    void init_bar(Bar& bar, const TickRecord& tick, long start_ms) {
        strncpy(bar.symbol, tick.symbol, 31);
        bar.start_time = start_ms;
        bar.open = tick.last_price;
        bar.high = tick.last_price;
        bar.low = tick.last_price;
        bar.close = tick.last_price;
        bar.volume = 0;
        bar.turnover = 0;
        
        bar.last_tick_vol = tick.volume;
        bar.last_tick_turnover = tick.turnover;
        bar.initialized = true;
    }

    void finish_bar(const Bar& bar) {
        // 格式化时间 HH:MM:00
        long s = bar.start_time / 1000;
        int hh = s / 3600;
        int mm = (s % 3600) / 60;
        
        std::cout << bar.symbol << ","
                  << std::setfill('0') << std::setw(2) << hh << ":" 
                  << std::setw(2) << mm << ":00,"
                  << std::fixed << std::setprecision(2)
                  << bar.open << ","
                  << bar.high << ","
                  << bar.low << ","
                  << bar.close << ","
                  << bar.volume << ","
                  << bar.turnover
                  << std::endl;
    }

    int interval_ms_;
    std::unordered_map<std::string, Bar> context_;
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <dat_file> [interval_min]" << std::endl;
        return 1;
    }

    std::string file_path = argv[1];
    int interval = 1;
    if (argc > 2) interval = std::stoi(argv[2]);

    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs) {
        std::cerr << "Error: Cannot open " << file_path << std::endl;
        return 1;
    }

    std::cout << "Symbol,Time,Open,High,Low,Close,Volume,Turnover" << std::endl;

    BarGenerator bg(interval);
    TickRecord rec;
    
    while (ifs.read(reinterpret_cast<char*>(&rec), sizeof(TickRecord))) {
        bg.process_tick(rec);
    }
    bg.finish_all();

    return 0;
}
