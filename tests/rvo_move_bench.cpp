#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {

struct BenchResult {
    double seconds;
    double ns_per_iter;
    uint64_t checksum;
};

class ExpensiveMoveBuffer {
public:
    explicit ExpensiveMoveBuffer(size_t n)
        : size_(n), data_(std::make_unique<uint64_t[]>(n)) {}

    ExpensiveMoveBuffer(ExpensiveMoveBuffer&& other) noexcept
        : size_(other.size_), data_(std::make_unique<uint64_t[]>(other.size_)) {
        std::memcpy(data_.get(), other.data_.get(), size_ * sizeof(uint64_t));
        other.size_ = 0;
    }

    ExpensiveMoveBuffer& operator=(ExpensiveMoveBuffer&& other) noexcept {
        if (this != &other) {
            size_ = other.size_;
            data_ = std::make_unique<uint64_t[]>(other.size_);
            std::memcpy(data_.get(), other.data_.get(), size_ * sizeof(uint64_t));
            other.size_ = 0;
        }
        return *this;
    }

    ExpensiveMoveBuffer(const ExpensiveMoveBuffer&) = delete;
    ExpensiveMoveBuffer& operator=(const ExpensiveMoveBuffer&) = delete;

    uint64_t& operator[](size_t i) noexcept { return data_[i]; }
    const uint64_t& operator[](size_t i) const noexcept { return data_[i]; }
    size_t size() const noexcept { return size_; }

private:
    size_t size_;
    std::unique_ptr<uint64_t[]> data_;
};

__attribute__((noinline)) ExpensiveMoveBuffer make_plain(size_t n, uint64_t seed) {
    ExpensiveMoveBuffer values(n);
    values[0] = seed;
    values[n - 1] = seed ^ 0x9e3779b97f4a7c15ULL;
    return values;
}

__attribute__((noinline)) ExpensiveMoveBuffer make_moved(size_t n, uint64_t seed) {
    ExpensiveMoveBuffer values(n);
    values[0] = seed;
    values[n - 1] = seed ^ 0x9e3779b97f4a7c15ULL;
    return std::move(values);
}

template <typename Factory>
BenchResult run_case(size_t iterations, size_t elements, Factory&& factory) {
    uint64_t checksum = 0;
    const auto begin = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        auto values = factory(elements, static_cast<uint64_t>(i) + checksum);
        checksum ^= values[0] + values[values.size() - 1];
    }
    const auto end = std::chrono::steady_clock::now();

    const double seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(end - begin).count();
    const double ns_per_iter = seconds * 1e9 / static_cast<double>(iterations);
    return {seconds, ns_per_iter, checksum};
}

void print_result(const std::string& name, const BenchResult& result, size_t iterations) {
    std::cout << std::left << std::setw(16) << name
              << " iterations=" << iterations
              << " time=" << std::fixed << std::setprecision(6) << result.seconds << "s"
              << " ns/op=" << std::setprecision(3) << result.ns_per_iter
              << " checksum=" << result.checksum << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    size_t iterations = 2'000'000;
    size_t elements = 256;

    if (argc > 1) {
        iterations = static_cast<size_t>(std::strtoull(argv[1], nullptr, 10));
    }
    if (argc > 2) {
        elements = static_cast<size_t>(std::strtoull(argv[2], nullptr, 10));
    }
    if (iterations == 0 || elements < 2) {
        std::cerr << "usage: rvo_move_bench [iterations>0] [elements>=2]\n";
        return 1;
    }

    std::cout << "expensive-move return vs return std::move benchmark\n";
    std::cout << "iterations=" << iterations
              << " elements=" << elements
              << " bytes=" << (elements * sizeof(uint64_t)) << "\n\n";

    const auto plain = run_case(iterations, elements, make_plain);
    const auto moved = run_case(iterations, elements, make_moved);

    print_result("return_obj", plain, iterations);
    print_result("return_move", moved, iterations);

    std::cout << "\nratio move/plain="
              << std::fixed << std::setprecision(3)
              << (moved.ns_per_iter / plain.ns_per_iter) << "x\n";

    return 0;
}
