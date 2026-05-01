#include "core/include/mmap_util.h"

#include <cstddef>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

std::string temp_base(const std::string& name) {
    return "/tmp/hft_eb_" + name + "_" + std::to_string(getpid());
}

void cleanup(const std::string& base) {
    unlink((base + ".dat").c_str());
    unlink((base + ".meta").c_str());
}

TickRecord make_tick(int i) {
    TickRecord rec {};
    std::snprintf(rec.symbol, sizeof(rec.symbol), "rb%04d", i);
    rec.symbol_id = static_cast<uint64_t>(1000 + i);
    rec.trading_day = 20260501;
    rec.update_time = static_cast<uint64_t>(90000000 + i);
    rec.last_price = 3000.0 + i;
    rec.volume = i * 10;
    return rec;
}

void assert_true(bool cond, const std::string& msg) {
    if (!cond) throw std::runtime_error(msg);
}

void test_tick_round_trip() {
    const std::string base = temp_base("tick_round_trip");
    cleanup(base);

    {
        MmapWriter<TickRecord> writer(base, 8);
        assert_true(writer.write(make_tick(1)), "write tick 1 failed");
        assert_true(writer.write(make_tick(2)), "write tick 2 failed");
        assert_true(writer.write(make_tick(3)), "write tick 3 failed");
    }

    MmapReader<TickRecord> reader(base);
    assert_true(reader.get_total_count() == 3, "tick count mismatch");

    TickRecord rec {};
    assert_true(reader.read(rec), "read tick 1 failed");
    assert_true(rec.symbol_id == 1001 && rec.last_price == 3001.0, "tick 1 mismatch");
    assert_true(reader.read(rec), "read tick 2 failed");
    assert_true(rec.symbol_id == 1002 && rec.volume == 20, "tick 2 mismatch");
    assert_true(reader.read(rec), "read tick 3 failed");
    assert_true(rec.symbol_id == 1003, "tick 3 mismatch");
    assert_true(!reader.read(rec), "reader returned extra tick");

    cleanup(base);
}

void test_kline_round_trip() {
    const std::string base = temp_base("kline_round_trip");
    cleanup(base);

    KlineRecord k {};
    std::strncpy(k.symbol, "rb2605", sizeof(k.symbol) - 1);
    k.symbol_id = 2001;
    k.trading_day = 20260501;
    k.start_time = 90000000;
    k.open = 1.0;
    k.high = 2.0;
    k.low = 0.5;
    k.close = 1.5;
    k.volume = 100;
    k.interval = K_1M;

    {
        MmapWriter<KlineRecord> writer(base, 4);
        assert_true(writer.write(k), "write kline failed");
    }

    MmapReader<KlineRecord> reader(base);
    KlineRecord out {};
    assert_true(reader.get_total_count() == 1, "kline count mismatch");
    assert_true(reader.read(out), "read kline failed");
    assert_true(out.symbol_id == k.symbol_id && out.interval == K_1M, "kline mismatch");

    cleanup(base);
}

void test_meta_validation_failure() {
    const std::string base = temp_base("meta_validation");
    cleanup(base);

    {
        MmapWriter<TickRecord> writer(base, 2);
        assert_true(writer.write(make_tick(1)), "write validation tick failed");
    }

    std::fstream meta(base + ".meta", std::ios::binary | std::ios::in | std::ios::out);
    assert_true(static_cast<bool>(meta), "open meta for tamper failed");
    const uint32_t bad_record_size = 1;
    meta.seekp(offsetof(MetaHeader, record_size));
    meta.write(reinterpret_cast<const char*>(&bad_record_size), sizeof(bad_record_size));
    meta.close();

    bool threw = false;
    try {
        MmapReader<TickRecord> reader(base);
    } catch (const std::exception&) {
        threw = true;
    }
    assert_true(threw, "tampered meta did not fail validation");

    cleanup(base);
}

}  // namespace

int main() {
    try {
        test_tick_round_trip();
        test_kline_round_trip();
        test_meta_validation_failure();
    } catch (const std::exception& e) {
        std::cerr << "mmap_format_test failed: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "mmap_format_test passed" << std::endl;
    return 0;
}
