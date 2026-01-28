#include "protocol.h"
#include "mmap_util.h"
#include <iostream>
#include <iomanip>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "用法: " << argv[0] << " <基础文件名(不带后缀)>" << std::endl;
        return 1;
    }

    std::string base_path = argv[1];
    
    try {
        std::cout << "正在映射文件: " << base_path << "..." << std::endl;
        MmapReader<TickRecord> reader(base_path);
        
        TickRecord rec;
        size_t count = 0;

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "----------------------------------------------------------------" << std::endl;
        std::cout << "IDX | 合约   | 交易日   | 时间         | 价格    | 成交量 | 成交额" << std::endl;
        std::cout << "----------------------------------------------------------------" << std::endl;

        while (reader.read(rec)) {
            std::cout << std::setw(3) << count++ << " | "
                      << std::setw(6) << rec.symbol << " | "
                      << rec.trading_day << " | "
                      << rec.update_time << " | "
                      << std::setw(7) << rec.last_price << " | "
                      << std::setw(6) << rec.volume << " | "
                      << rec.turnover 
                      << std::endl;
        }

        std::cout << "----------------------------------------------------------------" << std::endl;
        std::cout << "总计记录: " << count << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
