#pragma once
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <string>
#include <stdexcept>
#include <iostream>

// 元数据头 (4KB 对齐)
struct MetaHeader {
    std::atomic<uint64_t> write_cursor; // 已写入条数
    uint64_t capacity;                  // 总容量 (条数)
    char padding[4096 - 16];            // 补齐，避免伪共享
};

// ---------------------------------------------------------
// Mmap 写入器 (单生产者)
// ---------------------------------------------------------
template <typename T>
class MmapWriter {
public:
    // capacity: 能够存储的记录总数
    MmapWriter(const std::string& base_path, uint64_t capacity) {
        std::string dat_path = base_path + ".dat";
        std::string meta_path = base_path + ".meta";

        // 1. 打开/创建数据文件
        int fd_dat = open(dat_path.c_str(), O_RDWR | O_CREAT, 0666);
        if (fd_dat < 0) throw std::runtime_error("无法打开数据文件: " + dat_path);
        
        // 预分配空间
        uint64_t dat_size = capacity * sizeof(T);
        if (ftruncate(fd_dat, dat_size) != 0) throw std::runtime_error("ftruncate 数据文件失败");

        data_ptr_ = (T*)mmap(nullptr, dat_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_dat, 0);
        if (data_ptr_ == MAP_FAILED) throw std::runtime_error("mmap 数据文件失败");
        close(fd_dat);

        // 2. 打开/创建元数据文件
        int fd_meta = open(meta_path.c_str(), O_RDWR | O_CREAT, 0666);
        if (fd_meta < 0) throw std::runtime_error("无法打开元数据文件: " + meta_path);
        
        if (ftruncate(fd_meta, sizeof(MetaHeader)) != 0) throw std::runtime_error("ftruncate 元数据文件失败");

        meta_ptr_ = (MetaHeader*)mmap(nullptr, sizeof(MetaHeader), PROT_READ | PROT_WRITE, MAP_SHARED, fd_meta, 0);
        if (meta_ptr_ == MAP_FAILED) throw std::runtime_error("mmap 元数据文件失败");
        close(fd_meta);

        // 初始化元数据
        if (meta_ptr_->capacity == 0) {
            meta_ptr_->capacity = capacity;
            meta_ptr_->write_cursor = 0;
        }
    }

    bool write(const T& record) {
        uint64_t cursor = meta_ptr_->write_cursor.load(std::memory_order_relaxed);
        if (cursor >= meta_ptr_->capacity) return false;

        // 1. 拷贝数据
        data_ptr_[cursor] = record;
        
        // 2. 内存屏障，确保数据先于游标可见
        std::atomic_thread_fence(std::memory_order_release);

        // 3. 更新游标
        meta_ptr_->write_cursor.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

private:
    T* data_ptr_ = nullptr;
    MetaHeader* meta_ptr_ = nullptr;
};

// ---------------------------------------------------------
// Mmap 读取器 (多消费者)
// ---------------------------------------------------------
template <typename T>
class MmapReader {
public:
    MmapReader(const std::string& base_path) {
        std::string dat_path = base_path + ".dat";
        std::string meta_path = base_path + ".meta";

        // 1. 打开元数据 (只读)
        int fd_meta = open(meta_path.c_str(), O_RDONLY);
        if (fd_meta < 0) throw std::runtime_error("无法打开元数据文件: " + meta_path);
        
        meta_ptr_ = (MetaHeader*)mmap(nullptr, sizeof(MetaHeader), PROT_READ, MAP_SHARED, fd_meta, 0);
        if (meta_ptr_ == MAP_FAILED) throw std::runtime_error("mmap 元数据文件失败");
        close(fd_meta);

        // 2. 打开数据 (只读)
        int fd_dat = open(dat_path.c_str(), O_RDONLY);
        if (fd_dat < 0) throw std::runtime_error("无法打开数据文件: " + dat_path);
        
        uint64_t dat_size = meta_ptr_->capacity * sizeof(T);
        data_ptr_ = (T*)mmap(nullptr, dat_size, PROT_READ, MAP_SHARED, fd_dat, 0);
        if (data_ptr_ == MAP_FAILED) throw std::runtime_error("mmap 数据文件失败");
        close(fd_dat);
        
        local_cursor_ = 0; 
    }

    bool read(T& out_record) {
        uint64_t w_cursor = meta_ptr_->write_cursor.load(std::memory_order_acquire);
        
        if (local_cursor_ >= w_cursor) {
            return false;
        }

        out_record = data_ptr_[local_cursor_];
        local_cursor_++;
        return true;
    }

    void seek_to_end() {
        local_cursor_ = meta_ptr_->write_cursor.load(std::memory_order_acquire);
    }
    
    void seek_to_start() {
        local_cursor_ = 0;
    }

private:
    T* data_ptr_ = nullptr;
    MetaHeader* meta_ptr_ = nullptr;
    uint64_t local_cursor_ = 0;
};
