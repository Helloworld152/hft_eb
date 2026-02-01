#include "protocol.h"
#include "mmap_util.h"
#include <iostream>
#include <iomanip>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <kline_dat_base_path>" << std::endl;
        std::cerr << "Example: " << argv[0] << " ../../data/kline_1m_20260130" << std::endl;
        return 1;
    }

    std::string base_path = argv[1];
    
    try {
        std::cout << "Mapping K-line file: " << base_path << "..." << std::endl;
        MmapReader<KlineRecord> reader(base_path);
        
        KlineRecord rec;
        uint64_t total = reader.get_total_count();
        uint64_t start_pos = (total > 100) ? (total - 100) : 0;
        
        reader.seek(start_pos);
        size_t count = start_pos;

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "----------------------------------------------------------------------------------------------------" << std::endl;
        std::cout << "IDX | Symbol | ID       | Day      | StartTime | Interval | Open    | High    | Low     | Close   | Vol" << std::endl;
        std::cout << "----------------------------------------------------------------------------------------------------" << std::endl;

        while (reader.read(rec)) {
            std::string interval_str = "UNK";
            if (rec.interval == K_1M) interval_str = "1M";
            else if (rec.interval == K_1H) interval_str = "1H";
            else if (rec.interval == K_1D) interval_str = "1D";

            std::cout << std::setw(3) << count++ << " | "
                      << std::setw(6) << rec.symbol << " | "
                      << std::setw(8) << rec.symbol_id << " | "
                      << rec.trading_day << " | "
                      << std::setw(9) << rec.start_time << " | "
                      << std::setw(8) << interval_str << " | "
                      << std::setw(7) << rec.open << " | "
                      << std::setw(7) << rec.high << " | "
                      << std::setw(7) << rec.low << " | "
                      << std::setw(7) << rec.close << " | "
                      << rec.volume
                      << std::endl;
        }

        std::cout << "----------------------------------------------------------------------------------------------------" << std::endl;
        std::cout << "Total Records: " << total << " (Showing last " << (total - start_pos) << ")" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
