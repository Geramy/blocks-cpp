#ifndef BLOCK_DEVICE_H
#define BLOCK_DEVICE_H

#include "blocks_types.h"
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <optional>

namespace blocks {

class BlockDevice {
public:
    BlockDevice(const std::string& devpath);
    
    static BlockDevice by_uuid(const std::string& uuid);
    
    int open_excl();
    
    class ExclusiveFileDescriptor {
    public:
        ExclusiveFileDescriptor(int fd) : fd(fd) {}
        ~ExclusiveFileDescriptor() { if (fd >= 0) close(fd); }
        operator int() const { return fd; }
    private:
        int fd;
    };
    
    ExclusiveFileDescriptor open_excl_ctx();
    
    std::string ptable_type();
    std::string superblock_type();
    std::string superblock_at(uint64_t offset);
    bool has_bcache_superblock();
    uint64_t size();
    void reset_size();
    
    std::string sysfspath();
    std::pair<int, int> devnum();
    std::vector<BlockDevice> iter_holders();
    
    bool is_dm();
    bool is_lv();
    
    std::string dm_table();
    void dm_deactivate();
    void dm_setup(const std::string& table, bool readonly);
    
    bool is_partition();
    std::pair<class PartitionTable, uint64_t> ptable_context();
    
    void dev_resize(uint64_t newsize, bool shrink);
    
    std::string devpath;
    
private:
    std::unordered_map<std::string, std::string> memoized_strings;
    std::unordered_map<std::string, uint64_t> memoized_uint64;
    std::unordered_map<std::string, bool> memoized_bools;

    memoized_property<std::string, BlockDevice> _ptable_type;
    memoized_property<std::string, BlockDevice> _superblock_type;
    memoized_property<bool, BlockDevice> _has_bcache_superblock;
    memoized_property<uint64_t, BlockDevice> _size;
    memoized_property<bool, BlockDevice> _is_dm;
    memoized_property<bool, BlockDevice> _is_lv;
    memoized_property<bool, BlockDevice> _is_partition;
};

class PartitionedDevice : public BlockDevice {
public:
    PartitionedDevice(const std::string& devpath);
    
    void* parted_device();
    
private:
    std::unordered_map<std::string, void*> memoized_pointers;
    memoized_property<void*, PartitionedDevice> _parted_device;
};

    class BlockData {
    public:
        explicit BlockData(BlockDevice device);
        virtual ~BlockData() = default;  // Ensures polymorphism
        BlockDevice device;
    };

class PartitionTable : public BlockData {
public:
    PartitionTable(PartitionedDevice device, void* parted_disk);
    
    static PartitionTable mkgpt(BlockDevice device);
    
    std::vector<void*> _iter_range(uint64_t start_sector, uint64_t end_sector);
    void _reserve_range(uint64_t start, uint64_t end, ProgressListener& progress);
    void reserve_space_before(uint64_t part_start, uint64_t length, ProgressListener& progress);
    void part_resize(uint64_t part_start, uint64_t newsize, bool shrink);
    void shift_left(uint64_t part_start, uint64_t part_start1);
    
    PartitionedDevice device;
    void* parted_disk;
};

class BlockStack;

BlockStack get_block_stack(BlockDevice device, ProgressListener& progress);

} // namespace blocks

#endif // BLOCK_DEVICE_H
