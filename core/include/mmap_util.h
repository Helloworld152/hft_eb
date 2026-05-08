#pragma once

#include "protocol.h"
#include "../../include/logging.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace mmap_format {

constexpr uint64_t DAT_MAGIC = 0x4846544441543031ULL;   // "HFTDAT01"
constexpr uint64_t META_MAGIC = 0x4846544D45544131ULL;  // "HFTMETA1"
constexpr uint32_t VERSION = 1;
constexpr uint32_t HEADER_SIZE = 4096;
constexpr uint32_t STATE_OPEN = 1;
constexpr uint32_t STATE_CLOSED = 2;
constexpr uint32_t RECORD_KIND_TICK = 1;
constexpr uint32_t RECORD_KIND_KLINE = 2;

template <typename T>
struct MmapRecordTraits {
    static_assert(sizeof(T) == 0, "MmapRecordTraits must be specialized for this record type");
};

template <>
struct MmapRecordTraits<TickRecord> {
    static constexpr uint32_t kind = RECORD_KIND_TICK;
};

template <>
struct MmapRecordTraits<KlineRecord> {
    static constexpr uint32_t kind = RECORD_KIND_KLINE;
};

struct MmapDatHeader {
    uint64_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t record_kind;
    uint32_t record_size;
    uint32_t record_align;
    uint32_t reserved0;
    uint64_t capacity;
    char reserved[HEADER_SIZE - 40];
};

struct MetaHeader {
    uint64_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t record_kind;
    uint32_t record_size;
    uint32_t state;
    uint32_t reserved0;
    uint64_t capacity;
    std::atomic<uint64_t> write_cursor;
    char reserved[HEADER_SIZE - 48];
};

static_assert(sizeof(MmapDatHeader) == HEADER_SIZE, "MmapDatHeader must be 4KB");
static_assert(sizeof(MetaHeader) == HEADER_SIZE, "MetaHeader must be 4KB");

inline uint64_t file_size_or_throw(int fd, const std::string& path) {
    struct stat st {};
    if (fstat(fd, &st) != 0) {
        throw std::runtime_error("无法读取文件大小: " + path);
    }
    if (st.st_size < 0) {
        throw std::runtime_error("文件大小非法: " + path);
    }
    return static_cast<uint64_t>(st.st_size);
}

template <typename T>
class MmapFileValidator {
public:
    static constexpr uint32_t record_kind = MmapRecordTraits<T>::kind;
    static constexpr uint32_t record_size = sizeof(T);
    static constexpr uint32_t record_align = alignof(T);

    static void init_dat_header(MmapDatHeader* header, uint64_t capacity) {
        std::memset(header, 0, sizeof(MmapDatHeader));
        header->magic = DAT_MAGIC;
        header->version = VERSION;
        header->header_size = HEADER_SIZE;
        header->record_kind = record_kind;
        header->record_size = record_size;
        header->record_align = record_align;
        header->capacity = capacity;
    }

    static void init_meta_header(MetaHeader* header, uint64_t capacity) {
        std::memset(header, 0, sizeof(MetaHeader));
        header->magic = META_MAGIC;
        header->version = VERSION;
        header->header_size = HEADER_SIZE;
        header->record_kind = record_kind;
        header->record_size = record_size;
        header->state = STATE_OPEN;
        header->capacity = capacity;
        header->write_cursor.store(0, std::memory_order_relaxed);
    }

    static void validate_dat_header(const MmapDatHeader& header, uint64_t expected_capacity) {
        if (header.magic != DAT_MAGIC) throw std::runtime_error("dat magic 校验失败");
        if (header.version != VERSION) throw std::runtime_error("dat version 校验失败");
        if (header.header_size != HEADER_SIZE) throw std::runtime_error("dat header_size 校验失败");
        if (header.record_kind != record_kind) throw std::runtime_error("dat record_kind 校验失败");
        if (header.record_size != record_size) throw std::runtime_error("dat record_size 校验失败");
        if (header.record_align != record_align) throw std::runtime_error("dat record_align 校验失败");
        if (header.capacity != expected_capacity) throw std::runtime_error("dat capacity 校验失败");
    }

    static void validate_meta_header(const MetaHeader& header) {
        if (header.magic != META_MAGIC) throw std::runtime_error("meta magic 校验失败");
        if (header.version != VERSION) throw std::runtime_error("meta version 校验失败");
        if (header.header_size != HEADER_SIZE) throw std::runtime_error("meta header_size 校验失败");
        if (header.record_kind != record_kind) throw std::runtime_error("meta record_kind 校验失败");
        if (header.record_size != record_size) throw std::runtime_error("meta record_size 校验失败");
        if (header.state != STATE_OPEN && header.state != STATE_CLOSED) {
            throw std::runtime_error("meta state 校验失败");
        }
        if (header.capacity == 0) throw std::runtime_error("meta capacity 校验失败");
    }

    static void validate_pair(const MmapDatHeader& dat_header,
                              const MetaHeader& meta_header,
                              uint64_t dat_file_size) {
        validate_meta_header(meta_header);
        validate_dat_header(dat_header, meta_header.capacity);

        uint64_t write_cursor = meta_header.write_cursor.load(std::memory_order_acquire);
        if (write_cursor > meta_header.capacity) {
            throw std::runtime_error("meta write_cursor 超过 capacity");
        }

        const uint64_t closed_size = HEADER_SIZE + write_cursor * record_size;
        const uint64_t open_size = HEADER_SIZE + meta_header.capacity * record_size;
        if (meta_header.state == STATE_CLOSED) {
            if (dat_file_size != closed_size) {
                throw std::runtime_error("closed dat 文件大小校验失败");
            }
        } else {
            if (dat_file_size != open_size) {
                throw std::runtime_error("open dat 文件大小校验失败");
            }
        }
    }
};

}  // namespace mmap_format

using MetaHeader = mmap_format::MetaHeader;

// ---------------------------------------------------------
// Mmap 写入器 (单生产者)
// ---------------------------------------------------------
template <typename T>
class MmapWriter {
public:
    MmapWriter(const std::string& base_path, uint64_t capacity, bool prefault = false)
        : base_path_(base_path), capacity_(capacity), prefault_(prefault) {
        if (capacity_ == 0) {
            throw std::runtime_error("MmapWriter capacity 不能为 0");
        }

        std::string dat_path = base_path + ".dat";
        std::string meta_path = base_path + ".meta";
        const uint64_t dat_size = mmap_format::HEADER_SIZE + capacity_ * sizeof(T);

        int fd_meta = open(meta_path.c_str(), O_RDWR | O_CREAT, 0666);
        if (fd_meta < 0) throw std::runtime_error("无法打开元数据文件: " + meta_path);
        uint64_t existing_meta_size = mmap_format::file_size_or_throw(fd_meta, meta_path);

        if (existing_meta_size == 0) {
            if (ftruncate(fd_meta, sizeof(MetaHeader)) != 0) {
                close(fd_meta);
                throw std::runtime_error("ftruncate 元数据文件失败");
            }
        } else if (existing_meta_size != sizeof(MetaHeader)) {
            close(fd_meta);
            throw std::runtime_error("元数据文件大小校验失败: " + meta_path);
        }

        meta_ptr_ = reinterpret_cast<MetaHeader*>(
            mmap(nullptr, sizeof(MetaHeader), PROT_READ | PROT_WRITE, MAP_SHARED, fd_meta, 0));
        if (meta_ptr_ == MAP_FAILED) {
            close(fd_meta);
            meta_ptr_ = nullptr;
            throw std::runtime_error("mmap 元数据文件失败");
        }
        close(fd_meta);

        if (existing_meta_size == 0) {
            mmap_format::MmapFileValidator<T>::init_meta_header(meta_ptr_, capacity_);
        } else {
            mmap_format::MmapFileValidator<T>::validate_meta_header(*meta_ptr_);
            if (meta_ptr_->capacity != capacity_) {
                throw std::runtime_error("元数据容量与写入容量不匹配: " + meta_path);
            }
        }

        int fd_dat = open(dat_path.c_str(), O_RDWR | O_CREAT, 0666);
        if (fd_dat < 0) throw std::runtime_error("无法打开数据文件: " + dat_path);
        uint64_t existing_dat_size = mmap_format::file_size_or_throw(fd_dat, dat_path);

        if (existing_dat_size == 0) {
            if (ftruncate(fd_dat, dat_size) != 0) {
                close(fd_dat);
                throw std::runtime_error("ftruncate 数据文件失败");
            }
        } else {
            mmap_format::MmapDatHeader existing_header {};
            if (existing_dat_size < sizeof(existing_header) ||
                pread(fd_dat, &existing_header, sizeof(existing_header), 0) !=
                    static_cast<ssize_t>(sizeof(existing_header))) {
                close(fd_dat);
                throw std::runtime_error("读取 dat header 失败: " + dat_path);
            }
            mmap_format::MmapFileValidator<T>::validate_dat_header(existing_header, capacity_);
            if (ftruncate(fd_dat, dat_size) != 0) {
                close(fd_dat);
                throw std::runtime_error("ftruncate 数据文件失败");
            }
        }

        dat_map_size_ = dat_size;
        dat_map_ptr_ = mmap(nullptr, dat_map_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_dat, 0);
        if (dat_map_ptr_ == MAP_FAILED) {
            close(fd_dat);
            dat_map_ptr_ = nullptr;
            throw std::runtime_error("mmap 数据文件失败");
        }
        close(fd_dat);

        auto* dat_header = reinterpret_cast<mmap_format::MmapDatHeader*>(dat_map_ptr_);
        data_ptr_ = reinterpret_cast<T*>(static_cast<char*>(dat_map_ptr_) + mmap_format::HEADER_SIZE);
        if (existing_dat_size == 0) {
            mmap_format::MmapFileValidator<T>::init_dat_header(dat_header, capacity_);
        }
        meta_ptr_->state = mmap_format::STATE_OPEN;

        if (prefault_) {
            volatile char* p = reinterpret_cast<volatile char*>(dat_map_ptr_);
            for (uint64_t i = 0; i < dat_map_size_; i += 4096) {
                p[i] = p[i];
            }
        }
    }

    ~MmapWriter() {
        if (meta_ptr_ && data_ptr_) {
            uint64_t final_cursor = meta_ptr_->write_cursor.load(std::memory_order_relaxed);
            uint64_t actual_size = mmap_format::HEADER_SIZE + final_cursor * sizeof(T);
            std::atomic_thread_fence(std::memory_order_release);

            munmap(dat_map_ptr_, dat_map_size_);

            std::string dat_path = base_path_ + ".dat";
            if (truncate(dat_path.c_str(), actual_size) != 0) {
                perror("MmapWriter truncate failed");
            } else {
                LOG_INFO("[MmapWriter] File truncated to {} records", final_cursor);
            }

            meta_ptr_->state = mmap_format::STATE_CLOSED;
            std::atomic_thread_fence(std::memory_order_release);
            munmap(meta_ptr_, sizeof(MetaHeader));
        }
    }

    bool write(const T& record) {
        uint64_t cursor = meta_ptr_->write_cursor.load(std::memory_order_relaxed);
        if (cursor >= meta_ptr_->capacity) return false;

        data_ptr_[cursor] = record;
        std::atomic_thread_fence(std::memory_order_release);
        meta_ptr_->write_cursor.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

private:
    std::string base_path_;
    uint64_t capacity_;
    void* dat_map_ptr_ = nullptr;
    uint64_t dat_map_size_ = 0;
    T* data_ptr_ = nullptr;
    MetaHeader* meta_ptr_ = nullptr;
    bool prefault_ = false;
};

// ---------------------------------------------------------
// Mmap 读取器 (多消费者)
// ---------------------------------------------------------
template <typename T>
class MmapReader {
public:
    MmapReader(const std::string& base_path, uint64_t max_capacity = 0) {
        (void)max_capacity;
        std::string dat_path = base_path + ".dat";
        std::string meta_path = base_path + ".meta";

        int fd_meta = open(meta_path.c_str(), O_RDONLY);
        if (fd_meta < 0) throw std::runtime_error("无法打开元数据文件: " + meta_path);
        uint64_t meta_size = mmap_format::file_size_or_throw(fd_meta, meta_path);
        if (meta_size != sizeof(MetaHeader)) {
            close(fd_meta);
            throw std::runtime_error("元数据文件大小校验失败: " + meta_path);
        }

        meta_ptr_ = reinterpret_cast<MetaHeader*>(
            mmap(nullptr, sizeof(MetaHeader), PROT_READ, MAP_SHARED, fd_meta, 0));
        if (meta_ptr_ == MAP_FAILED) {
            close(fd_meta);
            meta_ptr_ = nullptr;
            throw std::runtime_error("mmap 元数据文件失败");
        }
        close(fd_meta);

        int fd_dat = open(dat_path.c_str(), O_RDONLY);
        if (fd_dat < 0) {
            munmap(const_cast<MetaHeader*>(meta_ptr_), sizeof(MetaHeader));
            meta_ptr_ = nullptr;
            throw std::runtime_error("无法打开数据文件: " + dat_path);
        }
        dat_map_size_ = mmap_format::file_size_or_throw(fd_dat, dat_path);
        if (dat_map_size_ < mmap_format::HEADER_SIZE) {
            close(fd_dat);
            munmap(const_cast<MetaHeader*>(meta_ptr_), sizeof(MetaHeader));
            meta_ptr_ = nullptr;
            throw std::runtime_error("数据文件头不完整: " + dat_path);
        }

        dat_map_ptr_ = mmap(nullptr, dat_map_size_, PROT_READ, MAP_SHARED, fd_dat, 0);
        if (dat_map_ptr_ == MAP_FAILED) {
            close(fd_dat);
            dat_map_ptr_ = nullptr;
            munmap(const_cast<MetaHeader*>(meta_ptr_), sizeof(MetaHeader));
            meta_ptr_ = nullptr;
            throw std::runtime_error("mmap 数据文件失败");
        }
        close(fd_dat);

        auto* dat_header = reinterpret_cast<const mmap_format::MmapDatHeader*>(dat_map_ptr_);
        mmap_format::MmapFileValidator<T>::validate_pair(*dat_header, *meta_ptr_, dat_map_size_);

        capacity_ = meta_ptr_->capacity;
        data_ptr_ = reinterpret_cast<const T*>(static_cast<const char*>(dat_map_ptr_) + mmap_format::HEADER_SIZE);
        local_cursor_ = 0;
        cached_write_cursor_ = bounded_write_cursor();
    }

    ~MmapReader() {
        if (dat_map_ptr_) {
            munmap(dat_map_ptr_, dat_map_size_);
        }
        if (meta_ptr_) {
            munmap(const_cast<MetaHeader*>(meta_ptr_), sizeof(MetaHeader));
        }
    }

    bool read(T& out_record) {
        if (local_cursor_ >= cached_write_cursor_) {
            cached_write_cursor_ = bounded_write_cursor();
            if (local_cursor_ >= cached_write_cursor_) {
                return false;
            }
        }

        out_record = data_ptr_[local_cursor_];
        local_cursor_++;
        return true;
    }

    const T* read_ptr() {
        [[likely]] if (local_cursor_ < cached_write_cursor_) {
            const T* ptr = &data_ptr_[local_cursor_];
            local_cursor_++;
            if (local_cursor_ + 1 < cached_write_cursor_) {
                __builtin_prefetch(&data_ptr_[local_cursor_ + 1], 0, 3);
            }
            return ptr;
        }

        cached_write_cursor_ = bounded_write_cursor();
        if (local_cursor_ >= cached_write_cursor_) {
            return nullptr;
        }

        const T* ptr = &data_ptr_[local_cursor_];
        local_cursor_++;
        return ptr;
    }

    size_t read_batch(const T** out_ptrs, size_t max_count) {
        if (local_cursor_ >= cached_write_cursor_) {
            cached_write_cursor_ = bounded_write_cursor();
            if (local_cursor_ >= cached_write_cursor_) {
                return 0;
            }
        }

        size_t available = cached_write_cursor_ - local_cursor_;
        size_t count = (available < max_count) ? available : max_count;

        for (size_t i = 0; i < count; ++i) {
            out_ptrs[i] = &data_ptr_[local_cursor_ + i];
        }

        if (local_cursor_ + count + 8 < cached_write_cursor_) {
            __builtin_prefetch(&data_ptr_[local_cursor_ + count + 4], 0, 3);
        }

        local_cursor_ += count;
        return count;
    }

    void seek_to_end() {
        local_cursor_ = bounded_write_cursor();
        cached_write_cursor_ = local_cursor_;
    }

    void seek_to_start() {
        local_cursor_ = 0;
        cached_write_cursor_ = bounded_write_cursor();
    }

    uint64_t get_total_count() const {
        return bounded_write_cursor();
    }

    void seek(uint64_t pos) {
        uint64_t total = get_total_count();
        if (pos > total) pos = total;
        local_cursor_ = pos;
        cached_write_cursor_ = total;
    }

private:
    uint64_t bounded_write_cursor() const {
        uint64_t cursor = meta_ptr_->write_cursor.load(std::memory_order_acquire);
        if (cursor > capacity_) return capacity_;
        return cursor;
    }

    uint64_t capacity_ = 0;
    void* dat_map_ptr_ = nullptr;
    uint64_t dat_map_size_ = 0;
    const T* data_ptr_ = nullptr;
    const MetaHeader* meta_ptr_ = nullptr;
    uint64_t local_cursor_ = 0;
    uint64_t cached_write_cursor_ = 0;
};
