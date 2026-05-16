#include "framework.h"
#include "protocol.h"
#include "symbol_manager.h"

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

struct ParquetKlineRow {
    int yyyymmdd = 0;
    std::string symbol;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    int64_t volume = 0;
};

class KlineParquetReplayModule : public IModule {
public:
    void init(EventBus* bus, const ConfigMap& config, ITimerService* timer_svc = nullptr) override {
        bus_ = bus;
        (void)timer_svc;

        if (config.find("data_file") != config.end()) {
            data_file_ = config.at("data_file");
        }
        if (config.find("debug") != config.end()) {
            debug_ = (config.at("debug") == "true" || config.at("debug") == "1");
        }
        if (config.find("sleep_ms_per_bar") != config.end()) {
            sleep_ms_per_bar_ = std::stoi(config.at("sleep_ms_per_bar"));
        }
        if (config.find("start_time_hhmmssmmm") != config.end()) {
            start_time_hhmmssmmm_ = static_cast<uint64_t>(std::stoull(config.at("start_time_hhmmssmmm")));
        }
        if (config.find("stop_on_finish") != config.end()) {
            stop_on_finish_ = (config.at("stop_on_finish") == "true" || config.at("stop_on_finish") == "1");
        }
        if (config.find("use_close_x_volume") != config.end()) {
            use_close_x_volume_ = (config.at("use_close_x_volume") == "true" || config.at("use_close_x_volume") == "1");
        }
        if (config.find("start_date") != config.end()) {
            start_date_ = std::stoi(config.at("start_date"));
        }
        if (config.find("end_date") != config.end()) {
            end_date_ = std::stoi(config.at("end_date"));
        }

        LOG_INFO("[KlineParquetReplay] Initialized. File: {}", data_file_);
    }

    void start() override {
        running_ = true;
        thread_ = std::thread(&KlineParquetReplayModule::run, this);
    }

    void stop() override {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        LOG_INFO("[KlineParquetReplay] Stopped. Total bars: {}", bar_count_);
    }

private:
    static int yyyymmdd_from_timestamp_us(int64_t ts_us) {
        std::time_t sec = static_cast<std::time_t>(ts_us / 1000000);
        std::tm tm_utc;
#if defined(_WIN32)
        gmtime_s(&tm_utc, &sec);
#else
        gmtime_r(&sec, &tm_utc);
#endif
        int y = tm_utc.tm_year + 1900;
        int m = tm_utc.tm_mon + 1;
        int d = tm_utc.tm_mday;
        return y * 10000 + m * 100 + d;
    }

    bool load_parquet(std::vector<ParquetKlineRow>& out_rows) {
        if (data_file_.empty()) {
            LOG_ERROR("[KlineParquetReplay] data_file is empty.");
            return false;
        }

        auto maybe_file = arrow::io::ReadableFile::Open(data_file_);
        if (!maybe_file.ok()) {
            LOG_ERROR("[KlineParquetReplay] Failed to open file: {} | {}", data_file_, maybe_file.status().ToString());
            return false;
        }
        std::shared_ptr<arrow::io::ReadableFile> infile = *maybe_file;

        std::unique_ptr<parquet::arrow::FileReader> reader;
        auto st = parquet::arrow::OpenFile(infile, arrow::default_memory_pool(), &reader);
        if (!st.ok()) {
            LOG_ERROR("[KlineParquetReplay] Failed to open parquet reader: {}", st.ToString());
            return false;
        }

        std::shared_ptr<arrow::Table> table;
        st = reader->ReadTable(&table);
        if (!st.ok()) {
            LOG_ERROR("[KlineParquetReplay] Failed to read parquet table: {}", st.ToString());
            return false;
        }

        auto col_date = table->GetColumnByName("date");
        auto col_symbol = table->GetColumnByName("symbol");
        auto col_open = table->GetColumnByName("open");
        auto col_high = table->GetColumnByName("high");
        auto col_low = table->GetColumnByName("low");
        auto col_close = table->GetColumnByName("close");
        auto col_volume = table->GetColumnByName("volume");

        if (!col_date || !col_symbol || !col_open || !col_high || !col_low || !col_close || !col_volume) {
            LOG_ERROR("[KlineParquetReplay] Missing required columns in parquet file.");
            return false;
        }

        int num_chunks = col_date->num_chunks();
        out_rows.reserve(static_cast<size_t>(table->num_rows()));

        for (int c = 0; c < num_chunks; ++c) {
            auto date_arr = std::static_pointer_cast<arrow::TimestampArray>(col_date->chunk(c));
            auto symbol_arr = std::static_pointer_cast<arrow::LargeStringArray>(col_symbol->chunk(c));
            auto open_arr = std::static_pointer_cast<arrow::DoubleArray>(col_open->chunk(c));
            auto high_arr = std::static_pointer_cast<arrow::DoubleArray>(col_high->chunk(c));
            auto low_arr = std::static_pointer_cast<arrow::DoubleArray>(col_low->chunk(c));
            auto close_arr = std::static_pointer_cast<arrow::DoubleArray>(col_close->chunk(c));
            auto volume_arr = std::static_pointer_cast<arrow::Int64Array>(col_volume->chunk(c));

            int64_t len = date_arr->length();
            for (int64_t i = 0; i < len; ++i) {
                if (date_arr->IsNull(i) || symbol_arr->IsNull(i)) continue;
                int yyyymmdd = yyyymmdd_from_timestamp_us(date_arr->Value(i));
                if (start_date_ > 0 && yyyymmdd < start_date_) continue;
                if (end_date_ > 0 && yyyymmdd > end_date_) continue;

                ParquetKlineRow row;
                row.yyyymmdd = yyyymmdd;
                row.symbol = symbol_arr->GetString(i);
                row.open = open_arr->IsNull(i) ? 0.0 : open_arr->Value(i);
                row.high = high_arr->IsNull(i) ? 0.0 : high_arr->Value(i);
                row.low = low_arr->IsNull(i) ? 0.0 : low_arr->Value(i);
                row.close = close_arr->IsNull(i) ? 0.0 : close_arr->Value(i);
                row.volume = volume_arr->IsNull(i) ? 0 : volume_arr->Value(i);
                out_rows.emplace_back(std::move(row));
            }
        }

        return true;
    }

    void run() {
        std::vector<ParquetKlineRow> rows;
        if (!load_parquet(rows)) {
            LOG_ERROR("[KlineParquetReplay] Load parquet failed.");
            return;
        }

        std::sort(rows.begin(), rows.end(), [](const ParquetKlineRow& a, const ParquetKlineRow& b) {
            if (a.yyyymmdd != b.yyyymmdd) return a.yyyymmdd < b.yyyymmdd;
            return a.symbol < b.symbol;
        });

        LOG_INFO("[KlineParquetReplay] Loaded rows: {}", rows.size());

        for (const auto& row : rows) {
            if (!running_) break;

            KlineRecord k;
            std::memset(&k, 0, sizeof(KlineRecord));
            std::strncpy(k.symbol, row.symbol.c_str(), sizeof(k.symbol) - 1);
            k.symbol_id = SymbolManager::instance().get_id(k.symbol);
            k.trading_day = static_cast<uint32_t>(row.yyyymmdd);
            k.start_time = start_time_hhmmssmmm_;
            k.open = row.open;
            k.high = row.high;
            k.low = row.low;
            k.close = row.close;
            k.open_interest = 0.0;
            k.interval = K_1D;

            int volume = 0;
            if (row.volume > static_cast<int64_t>(std::numeric_limits<int>::max())) {
                if (!volume_clamped_warned_) {
                    LOG_WARN("[KlineParquetReplay] Volume exceeds INT_MAX, clamping.");
                    volume_clamped_warned_ = true;
                }
                volume = std::numeric_limits<int>::max();
            } else if (row.volume < 0) {
                volume = 0;
            } else {
                volume = static_cast<int>(row.volume);
            }
            k.volume = volume;
            k.turnover = use_close_x_volume_ ? (k.close * static_cast<double>(volume)) : 0.0;

            if (debug_ && (bar_count_ < 5 || bar_count_ % 10000 == 0)) {
                LOG_DEBUG("[KlineParquetReplay] #{} {} {} O:{} C:{} V:{}",
                          bar_count_,
                          k.symbol,
                          k.trading_day,
                          k.open,
                          k.close,
                          k.volume);
            }
            ++bar_count_;

            bus_->publish(EVENT_KLINE, &k);

            if (sleep_ms_per_bar_ > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms_per_bar_));
            }
        }

        if (stop_on_finish_) {
            bus_->publish(EVENT_ENGINE_STOP, nullptr);
        }
    }

    EventBus* bus_ = nullptr;
    std::string data_file_;

    std::thread thread_;
    std::atomic<bool> running_{false};

    bool debug_ = false;
    bool stop_on_finish_ = true;
    bool use_close_x_volume_ = false;
    bool volume_clamped_warned_ = false;
    int sleep_ms_per_bar_ = 0;
    int start_date_ = 0;
    int end_date_ = 0;
    uint64_t start_time_hhmmssmmm_ = 93000000ULL;

    uint64_t bar_count_ = 0;
};

EXPORT_MODULE(KlineParquetReplayModule)
