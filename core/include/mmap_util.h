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
    MmapWriter(const std::string& base_path, uint64_t capacity) 
        : base_path_(base_path), capacity_(capacity) {
        std::string dat_path = base_path + ".dat";
        std::string meta_path = base_path + ".meta";

        // 1. 打开/创建数据文件
        int fd_dat = open(dat_path.c_str(), O_RDWR | O_CREAT, 0666);
        if (fd_dat < 0) throw std::runtime_error("无法打开数据文件: " + dat_path);
        
        // 预分配空间
        uint64_t dat_size = capacity * sizeof(T);
        if (ftruncate(fd_dat, dat_size) != 0) {
            close(fd_dat);
            throw std::runtime_error("ftruncate 数据文件失败");
        }

        data_ptr_ = (T*)mmap(nullptr, dat_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_dat, 0);
        if (data_ptr_ == MAP_FAILED) {
            close(fd_dat);
            throw std::runtime_error("mmap 数据文件失败");
        }
        close(fd_dat);

        // 2. 打开/创建元数据文件
        int fd_meta = open(meta_path.c_str(), O_RDWR | O_CREAT, 0666);
        if (fd_meta < 0) throw std::runtime_error("无法打开元数据文件: " + meta_path);
        
        if (ftruncate(fd_meta, sizeof(MetaHeader)) != 0) {
            close(fd_meta);
            throw std::runtime_error("ftruncate 元数据文件失败");
        }

        meta_ptr_ = (MetaHeader*)mmap(nullptr, sizeof(MetaHeader), PROT_READ | PROT_WRITE, MAP_SHARED, fd_meta, 0);
        if (meta_ptr_ == MAP_FAILED) {
            close(fd_meta);
            throw std::runtime_error("mmap 元数据文件失败");
        }
        close(fd_meta);

        // 初始化元数据
        if (meta_ptr_->capacity == 0) {
            meta_ptr_->capacity = capacity;
            meta_ptr_->write_cursor = 0;
        }
    }

    ~MmapWriter() {
        if (meta_ptr_ && data_ptr_) {
            // 获取最终写入位置
            uint64_t final_cursor = meta_ptr_->write_cursor.load(std::memory_order_relaxed);
            uint64_t actual_size = final_cursor * sizeof(T);
            
            // 解除映射
            munmap(data_ptr_, capacity_ * sizeof(T));
            munmap(meta_ptr_, sizeof(MetaHeader));
            
            // 裁剪文件到实际大小，释放磁盘空间
            std::string dat_path = base_path_ + ".dat";
            if (truncate(dat_path.c_str(), actual_size) != 0) {
                perror("MmapWriter truncate failed");
            } else {
                std::cout << "[MmapWriter] File truncated to " << final_cursor << " records" << std::endl;
            }
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
    std::string base_path_;
    uint64_t capacity_;
    T* data_ptr_ = nullptr;
    MetaHeader* meta_ptr_ = nullptr;
};

// ---------------------------------------------------------
// Mmap 读取器 (多消费者)
// ---------------------------------------------------------
template <typename T>
class MmapReader {
public:
    // base_path: 数据文件基础路径
    // max_capacity: 最大容量（条数），0 表示使用 meta 文件中的 capacity（用于录制完成后读取）
    //               非 0 表示使用固定大小映射（用于实时读取场景）
    MmapReader(const std::string& base_path, uint64_t max_capacity = 0) {
        std::string dat_path = base_path + ".dat";
        std::string meta_path = base_path + ".meta";

        // 1. 打开元数据 (只读)
        int fd_meta = open(meta_path.c_str(), O_RDONLY);
        if (fd_meta < 0) throw std::runtime_error("无法打开元数据文件: " + meta_path);
        
        meta_ptr_ = (MetaHeader*)mmap(nullptr, sizeof(MetaHeader), PROT_READ, MAP_SHARED, fd_meta, 0);
        if (meta_ptr_ == MAP_FAILED) {
            close(fd_meta);
            throw std::runtime_error("mmap 元数据文件失败");
        }
        close(fd_meta);

        // 2. 打开数据 (只读)
        int fd_dat = open(dat_path.c_str(), O_RDONLY);
        if (fd_dat < 0) {
            munmap(meta_ptr_, sizeof(MetaHeader));
            throw std::runtime_error("无法打开数据文件: " + dat_path);
        }
        
        // 如果指定了 max_capacity，使用配置值；否则使用 meta 文件中的 capacity
        if (max_capacity > 0) {
            capacity_ = max_capacity;
        } else {
            capacity_ = meta_ptr_->capacity;
        }
        
        uint64_t dat_size = capacity_ * sizeof(T);
        data_ptr_ = (T*)mmap(nullptr, dat_size, PROT_READ, MAP_SHARED, fd_dat, 0);
        if (data_ptr_ == MAP_FAILED) {
            close(fd_dat);
            munmap(meta_ptr_, sizeof(MetaHeader));
            throw std::runtime_error("mmap 数据文件失败");
        }
        close(fd_dat);
        
        local_cursor_ = 0;
        cached_write_cursor_ = meta_ptr_->write_cursor.load(std::memory_order_acquire);
    }

    ~MmapReader() {
        if (data_ptr_) {
            munmap(data_ptr_, capacity_ * sizeof(T));
        }
        if (meta_ptr_) {
            munmap(meta_ptr_, sizeof(MetaHeader));
        }
    }

    bool read(T& out_record) {
        // 优化：使用缓存的 write_cursor
        if (local_cursor_ >= cached_write_cursor_) {
            cached_write_cursor_ = meta_ptr_->write_cursor.load(std::memory_order_acquire);
            if (local_cursor_ >= cached_write_cursor_) {
                return false;
            }
        }

        out_record = data_ptr_[local_cursor_];
        local_cursor_++;
        return true;
    }

    /** 返回 mmap 内记录的指针，无拷贝。调用方不得在下次 read/read_ptr 或 reader 析构后使用该指针。 */
    const T* read_ptr() {
        // 优化：缓存 write_cursor，减少原子操作频率
        // 只在 local_cursor_ 接近边界时才重新加载
        [[likely]] if (local_cursor_ < cached_write_cursor_) {
            const T* ptr = &data_ptr_[local_cursor_];
            local_cursor_++;
            
            // 预取下一条记录到 CPU 缓存（提前 1-2 条）
            if (local_cursor_ + 1 < cached_write_cursor_) {
                __builtin_prefetch(&data_ptr_[local_cursor_ + 1], 0, 3);  // 预取到 L1 缓存
            }
            return ptr;
        }
        
        // 边界检查：重新加载 write_cursor
        cached_write_cursor_ = meta_ptr_->write_cursor.load(std::memory_order_acquire);
        if (local_cursor_ >= cached_write_cursor_) {
            return nullptr;
        }
        
        const T* ptr = &data_ptr_[local_cursor_];
        local_cursor_++;
        return ptr;
    }
    
    /** 批量读取：一次读取多条记录到数组，返回实际读取数量 */
    size_t read_batch(const T** out_ptrs, size_t max_count) {
        if (local_cursor_ >= cached_write_cursor_) {
            cached_write_cursor_ = meta_ptr_->write_cursor.load(std::memory_order_acquire);
            if (local_cursor_ >= cached_write_cursor_) {
                return 0;
            }
        }
        
        size_t available = cached_write_cursor_ - local_cursor_;
        size_t count = (available < max_count) ? available : max_count;
        
        for (size_t i = 0; i < count; ++i) {
            out_ptrs[i] = &data_ptr_[local_cursor_ + i];
        }
        
        // 预取下一批数据
        if (local_cursor_ + count + 8 < cached_write_cursor_) {
            __builtin_prefetch(&data_ptr_[local_cursor_ + count + 4], 0, 3);
        }
        
        local_cursor_ += count;
        return count;
    }

    void seek_to_end() {
        local_cursor_ = meta_ptr_->write_cursor.load(std::memory_order_acquire);
        cached_write_cursor_ = local_cursor_;
    }
    
    void seek_to_start() {
        local_cursor_ = 0;
        cached_write_cursor_ = meta_ptr_->write_cursor.load(std::memory_order_acquire);
    }

    uint64_t get_total_count() const {
        return meta_ptr_->write_cursor.load(std::memory_order_acquire);
    }

    void seek(uint64_t pos) {
        uint64_t total = get_total_count();
        if (pos > total) pos = total;
        local_cursor_ = pos;
        cached_write_cursor_ = total;
    }

private:
    uint64_t capacity_;
    T* data_ptr_ = nullptr;
    MetaHeader* meta_ptr_ = nullptr;
    uint64_t local_cursor_ = 0;
    uint64_t cached_write_cursor_ = 0;  // 缓存的 write_cursor，减少原子操作
};
