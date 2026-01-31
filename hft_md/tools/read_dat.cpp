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
        uint64_t total = reader.get_total_count();
        uint64_t start_pos = (total > 100) ? (total - 100) : 0;
        
        reader.seek(start_pos);
        size_t count = start_pos;

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "----------------------------------------------------------------------------------" << std::endl;
        std::cout << "IDX | 合约   | ID       | 交易日   | 时间         | 价格    | 成交量 | 持仓量" << std::endl;
        std::cout << "----------------------------------------------------------------------------------" << std::endl;

        while (reader.read(rec)) {
            count++;
            std::cout << std::setw(3) << count << " | "
                      << std::setw(6) << rec.symbol << " | "
                      << std::setw(8) << rec.symbol_id << " | "
                      << rec.trading_day << " | "
                      << std::setw(12) << rec.update_time << " | "
                      << std::setw(7) << rec.last_price << " | "
                      << std::setw(6) << rec.volume << " | "
                      << std::setw(7) << rec.open_interest 
                      << std::endl;
        }

        std::cout << "----------------------------------------------------------------------------------" << std::endl;
        std::cout << "总计记录: " << total << " (展示最后 " << (total - start_pos) << " 条)" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
