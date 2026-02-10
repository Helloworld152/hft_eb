#include "protocol.h"
#include "mmap_util.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <getopt.h>

namespace fs = std::filesystem;

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options] <dir_or_file_base>" << std::endl;
    std::cerr << "Options:" << std::endl;
    std::cerr << "  -s, --symbol <id>    Filter by symbol (e.g., au2606)" << std::endl;
    std::cerr << "  -t, --start <time>   Start time (HHMMSSmmm, e.g., 093000000)" << std::endl;
    std::cerr << "  -e, --end <time>     End time (HHMMSSmmm, e.g., 150000000)" << std::endl;
    std::cerr << "  -d, --day <YYYYMMDD> Filter by trading day" << std::endl;
    std::cerr << "  -h, --help           Show this help" << std::endl;
}

void process_file(const std::string& base_path, const std::string& filter_symbol, 
                  uint64_t start_time, uint64_t end_time, uint32_t filter_day) {
    try {
        MmapReader<KlineRecord> reader(base_path);
        KlineRecord rec;
        
        while (reader.read(rec)) {
            // 过滤交易日
            if (filter_day != 0 && rec.trading_day != filter_day) continue;
            
            // 过滤 Symbol
            if (!filter_symbol.empty() && filter_symbol != rec.symbol) continue;
            
            // 过滤时间范围
            if (start_time != 0 && rec.start_time < start_time) continue;
            if (end_time != 0 && rec.start_time > end_time) continue;

            std::string interval_str = (rec.interval == K_1M) ? "1M" : 
                                       (rec.interval == K_1H) ? "1H" : 
                                       (rec.interval == K_1D) ? "1D" : "UNK";

            std::cout << std::setw(6) << rec.symbol << " | "
                      << rec.trading_day << " | "
                      << std::setfill('0') << std::setw(9) << rec.start_time << std::setfill(' ') << " | "
                      << std::setw(3) << interval_str << " | "
                      << std::setw(8) << rec.open << " | "
                      << std::setw(8) << rec.high << " | "
                      << std::setw(8) << rec.low << " | "
                      << std::setw(8) << rec.close << " | "
                      << std::setw(8) << rec.volume << " | "
                      << std::fixed << std::setprecision(0) << rec.turnover
                      << std::endl;
        }
    } catch (...) {
        // 忽略无法打开的文件
    }
}

int main(int argc, char* argv[]) {
    std::string filter_symbol;
    uint64_t start_time = 0;
    uint64_t end_time = 0;
    uint32_t filter_day = 0;

    static struct option long_options[] = {
        {"symbol", required_argument, 0, 's'},
        {"start",  required_argument, 0, 't'},
        {"end",    required_argument, 0, 'e'},
        {"day",    required_argument, 0, 'd'},
        {"help",   no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:t:e:d:h", long_options, nullptr)) != -1) {
        switch (opt) {
            case 's': filter_symbol = optarg; break;
            case 't': start_time = std::stoull(optarg); break;
            case 'e': end_time = std::stoull(optarg); break;
            case 'd': filter_day = std::stoul(optarg); break;
            case 'h': print_usage(argv[0]); return 0;
            default: print_usage(argv[0]); return 1;
        }
    }

    if (optind >= argc) {
        print_usage(argv[0]);
        return 1;
    }

    std::string path = argv[optind];

    std::cout << "Symbol | Day      | StartTime | Int | Open     | High     | Low      | Close    | Volume   | Turnover" << std::endl;
    std::cout << "-------|----------|-----------|-----|----------|----------|----------|----------|----------|----------" << std::endl;

    if (fs::is_directory(path)) {
        std::vector<std::string> files;
        for (const auto& entry : fs::directory_iterator(path)) {
            if (entry.path().extension() == ".meta") {
                std::string base = entry.path().string();
                files.push_back(base.substr(0, base.size() - 5));
            }
        }
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            process_file(f, filter_symbol, start_time, end_time, filter_day);
        }
    } else {
        process_file(path, filter_symbol, start_time, end_time, filter_day);
    }

    return 0;
}
