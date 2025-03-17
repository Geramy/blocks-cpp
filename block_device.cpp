#include "block_device.h"
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <filesystem>
#include <array>
#include <memory>
#include <sys/sysmacros.h>

namespace blocks {

    BlockDevice::BlockDevice(const std::string& devpath) :
            devpath(devpath),
            _ptable_type(&BlockDevice::ptable_type, "ptable_type"),
            _superblock_type(&BlockDevice::superblock_type, "superblock_type"),
            _has_bcache_superblock(&BlockDevice::has_bcache_superblock, "has_bcache_superblock"),
            _size(&BlockDevice::size, "size"),
            _is_dm(&BlockDevice::is_dm, "is_dm"),
            _is_lv(&BlockDevice::is_lv, "is_lv"),
            _is_partition(&BlockDevice::is_partition, "is_partition")
    {
        assert(std::filesystem::exists(devpath));
    }

BlockDevice BlockDevice::by_uuid(const std::string& uuid) {
    std::array<char, 256> buffer;
    std::string cmd = "blkid -U " + uuid;
    
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("Failed to execute blkid command");
    }
    
    std::string result;
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    
    pclose(pipe);
    
    // Remove trailing newline if present
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    
    return BlockDevice(result);
}

int BlockDevice::open_excl() {
    // O_EXCL on a block device takes the device lock,
    // exclusive against mounts and the like.
    // O_SYNC on a block device provides durability
    // O_DIRECT would bypass the block cache, which is irrelevant here
    return ::open(devpath.c_str(), O_SYNC | O_RDWR | O_EXCL);
}

BlockDevice::ExclusiveFileDescriptor BlockDevice::open_excl_ctx() {
    int fd = open_excl();
    if (fd < 0) {
        throw std::runtime_error("Failed to open device " + devpath + " exclusively: " + std::strerror(errno));
    }
    return ExclusiveFileDescriptor(fd);
}

std::string BlockDevice::ptable_type() {
    // TODO: also detect an MBR other than protective,
    // and refuse to edit that.
    std::vector<std::string> cmd = {"blkid", "-p", "-o", "value", "-s", "PTTYPE", "--", devpath};
    
    std::array<char, 256> buffer;
    std::string result;
    
    FILE* pipe = popen(("blkid -p -o value -s PTTYPE -- " + devpath).c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("Failed to execute blkid command");
    }
    
    if (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result = buffer.data();
        // Remove trailing newline if present
        if (!result.empty() && result.back() == '\n') {
            result.pop_back();
        }
    }
    
    pclose(pipe);
    return result;
}

std::string BlockDevice::superblock_type() {
    return superblock_at(0);
}

std::string BlockDevice::superblock_at(uint64_t offset) {
    std::string cmd = "blkid -p -o value -s TYPE -O " + std::to_string(offset) + " -- " + devpath;
    
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("Failed to execute blkid command");
    }
    
    std::array<char, 256> buffer;
    std::string result;
    
    if (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result = buffer.data();
        // Remove trailing newline if present
        if (!result.empty() && result.back() == '\n') {
            result.pop_back();
        }
    }
    
    int status = pclose(pipe);
    if (WEXITSTATUS(status) == 2) {
        // No recognised superblock
        return "";
    }
    
    return result;
}

bool BlockDevice::has_bcache_superblock() {
    // blkid doesn't detect bcache, so special-case it.
    // To keep dependencies light, don't use bcache-tools for detection,
    // only require the tools after a successful detection.
    if (size() <= 8192) {
        return false;
    }
    
    int sbfd = ::open(devpath.c_str(), O_RDONLY);
    if (sbfd < 0) {
        throw std::runtime_error("Failed to open device for bcache detection");
    }
    
    std::array<uint8_t, 16> magic;
    ssize_t bytes_read = ::pread(sbfd, magic.data(), magic.size(), 4096 + 24);
    ::close(sbfd);
    
    if (bytes_read != static_cast<ssize_t>(magic.size())) {
        return false;
    }
    
    return std::memcmp(magic.data(), BCACHE_MAGIC.data(), magic.size()) == 0;
}

uint64_t BlockDevice::size() {
    std::string cmd = "blockdev --getsize64 " + devpath;
    
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("Failed to execute blockdev command");
    }
    
    std::array<char, 256> buffer;
    std::string result;
    
    if (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result = buffer.data();
        // Remove trailing newline if present
        if (!result.empty() && result.back() == '\n') {
            result.pop_back();
        }
    }
    
    pclose(pipe);
    
    uint64_t size_value = std::stoull(result);
    assert(size_value % 512 == 0);
    return size_value;
}

void BlockDevice::reset_size() {
    _size.reset(this, memoized_uint64);
}

std::string BlockDevice::sysfspath() {
    // pyudev would also work
    struct stat st;
    if (::stat(devpath.c_str(), &st) != 0) {
        throw std::runtime_error("Failed to stat device " + devpath);
    }
    
    if (!S_ISBLK(st.st_mode)) {
        throw std::runtime_error(devpath + " is not a block device");
    }
    
    auto [major, minor] = devnum();
    return "/sys/dev/block/" + std::to_string(major) + ":" + std::to_string(minor);
}

std::pair<int, int> BlockDevice::devnum() {
    struct stat st;
    if (::stat(devpath.c_str(), &st) != 0) {
        throw std::runtime_error("Failed to stat device " + devpath);
    }
    
    if (!S_ISBLK(st.st_mode)) {
        throw std::runtime_error(devpath + " is not a block device");
    }
    
    return {major(st.st_rdev), minor(st.st_rdev)};
}

std::vector<BlockDevice> BlockDevice::iter_holders() {
    std::vector<BlockDevice> holders;
    std::string holders_path = sysfspath() + "/holders";
    
    if (!std::filesystem::exists(holders_path)) {
        return holders;
    }
    
    for (const auto& entry : std::filesystem::directory_iterator(holders_path)) {
        holders.emplace_back("/dev/" + entry.path().filename().string());
    }
    
    return holders;
}

bool BlockDevice::is_dm() {
    return std::filesystem::exists(sysfspath() + "/dm");
}

bool BlockDevice::is_lv() {
    if (!is_dm()) {
        return false;
    }
    
    try {
        std::string cmd = "lvm lvs --noheadings --rows --units=b --nosuffix "
                          "-o vg_extent_size -- " + devpath;
        
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            return false;
        }
        
        std::array<char, 256> buffer;
        std::string result;
        
        if (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            result = buffer.data();
            // Remove trailing whitespace
            result.erase(result.find_last_not_of(" \n\r\t") + 1);
            // Remove leading whitespace
            result.erase(0, result.find_first_not_of(" \n\r\t"));
        }
        
        int status = pclose(pipe);
        if (WEXITSTATUS(status) != 0) {
            return false;
        }
        
        // If we got a valid extent size, it's an LV
        std::stoull(result);
        return true;
    } catch (...) {
        return false;
    }
}

std::string BlockDevice::dm_table() {
    std::string cmd = "dmsetup table -- " + devpath;
    
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("Failed to execute dmsetup command");
    }
    
    std::array<char, 1024> buffer;
    std::string result;
    
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    
    pclose(pipe);
    return result;
}

void BlockDevice::dm_deactivate() {
    std::vector<std::string> cmd = {"dmsetup", "remove", "--", devpath};
    quiet_call(cmd);
}

void BlockDevice::dm_setup(const std::string& table, bool readonly) {
    std::vector<std::string> cmd = {"dmsetup", "create", "--", devpath};
    if (readonly) {
        cmd.insert(cmd.begin() + 2, "--readonly");
    }
    
    quiet_call(cmd, table);
}

bool BlockDevice::is_partition() {
    std::string partition_path = sysfspath() + "/partition";
    if (!std::filesystem::exists(partition_path)) {
        return false;
    }
    
    std::ifstream partition_file(partition_path);
    std::string content;
    std::getline(partition_file, content);
    
    return !content.empty() && content != "0";
}

std::pair<PartitionTable, uint64_t> BlockDevice::ptable_context() {
    // the outer ptable and our offset within that
    assert(is_partition());
    
    std::string parent_path = sysfspath() + "/..";
    PartitionedDevice ptable_device(devpath_from_sysdir(parent_path));
    
    std::ifstream start_file(sysfspath() + "/start");
    std::string start_str;
    std::getline(start_file, start_str);
    uint64_t part_start = std::stoull(start_str) * 512;
    
    // This will be implemented later when we have parted bindings
    void* parted_disk = nullptr; // placeholder for parted.disk.Disk(ptable_device.parted_device)
    
    return {PartitionTable(ptable_device, parted_disk), part_start};
}

void BlockDevice::dev_resize(uint64_t newsize, bool shrink) {
    newsize = align_up(newsize, 512);
    // Be explicit about the intended direction;
    // shrink is more dangerous
    if (is_partition()) {
        auto [ptable, part_start] = ptable_context();
        ptable.part_resize(part_start, newsize, shrink);
    } else if (is_lv()) {
        std::vector<std::string> cmd;
        if (shrink) {
            cmd = {"lvm", "lvreduce", "-f"};
        } else {
            // Alloc policy / dest PVs might be useful here,
            // but difficult to expose cleanly.
            // Just don't use --resize-device and do it manually.
            cmd = {"lvm", "lvextend"};
        }
        cmd.push_back("--size=" + std::to_string(newsize) + "b");
        cmd.push_back("--");
        cmd.push_back(devpath);
        
        quiet_call(cmd);
    } else {
        throw std::runtime_error("Only partitions and LVs can be resized");
    }
    reset_size();
}

PartitionedDevice::PartitionedDevice(const std::string& devpath) : 
    BlockDevice(devpath),
    _parted_device(&PartitionedDevice::parted_device,"parted_device")
{
}

void* PartitionedDevice::parted_device() {
    // This will be implemented later when we have parted bindings
    return nullptr; // placeholder for parted.device.Device(devpath)
}

BlockData::BlockData(BlockDevice device) : device(device) {
}

PartitionTable::PartitionTable(PartitionedDevice device, void* parted_disk) : 
    BlockData(device), device(device), parted_disk(parted_disk) {
}

PartitionTable PartitionTable::mkgpt(BlockDevice device) {
    // This will be implemented later when we have parted bindings
    PartitionedDevice ptable_device(device.devpath);
    void* parted_disk = nullptr; // placeholder for parted.freshDisk(ptable_device.parted_device, 'gpt')
    
    return PartitionTable(ptable_device, parted_disk);
}

std::vector<void*> PartitionTable::_iter_range(uint64_t start_sector, uint64_t end_sector) {
    // This will be implemented later when we have parted bindings
    return {}; // placeholder
}

void PartitionTable::_reserve_range(uint64_t start, uint64_t end, ProgressListener& progress) {
    // This will be implemented later when we have parted bindings
}

void PartitionTable::reserve_space_before(uint64_t part_start, uint64_t length, ProgressListener& progress) {
    // This will be implemented later when we have parted bindings
}

void PartitionTable::part_resize(uint64_t part_start, uint64_t newsize, bool shrink) {
    // This will be implemented later when we have parted bindings
}

void PartitionTable::shift_left(uint64_t part_start, uint64_t part_start1) {
    // This will be implemented later when we have parted bindings
}

} // namespace blocks
