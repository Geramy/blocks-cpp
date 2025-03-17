#include "filesystem.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace blocks {

Filesystem::Filesystem(BlockDevice device) : BlockData(device) {
}

uint64_t Filesystem::reserve_end_area_nonrec(uint64_t pos) {
    // align to a block boundary that doesn't encroach
    pos = align(pos, block_size);

    if (fssize() <= pos) {
        return pos;
    }

    if (!can_shrink()) {
        throw CantShrink();
    }

    _mount_and_resize(pos);
    return pos;
}

Filesystem::TempMount::TempMount(const std::string& devpath, const std::string& vfstype) {
    char template_path[] = "/tmp/privmnt-XXXXXX";
    char* temp_dir = mkdtemp(template_path);
    if (!temp_dir) {
        throw std::runtime_error("Failed to create temporary directory");
    }
    
    mpoint = temp_dir;
    this->devpath = devpath;
    
    std::vector<std::string> mount_cmd = {
        "mount", "-t", vfstype, "-o", "noatime,noexec,nodev",
        "--", devpath, mpoint
    };
    
    quiet_call(mount_cmd);
}

Filesystem::TempMount::~TempMount() {
    try {
        std::vector<std::string> umount_cmd = {"umount", "--", mpoint};
        quiet_call(umount_cmd);
        rmdir(mpoint.c_str());
    } catch (const std::exception& e) {
        std::cerr << "Error during unmount: " << e.what() << std::endl;
    }
}

std::unique_ptr<Filesystem::TempMount> Filesystem::temp_mount() {
    return std::make_unique<TempMount>(device.devpath, vfstype);
}

bool Filesystem::is_mounted() {
    auto [major, minor] = device.devnum();
    std::string device_id = std::to_string(major) + ":" + std::to_string(minor);
    
    std::ifstream mounts("/proc/self/mountinfo");
    std::string line;
    
    while (std::getline(mounts, line)) {
        std::istringstream iss(line);
        std::string item;
        std::vector<std::string> items;
        
        while (iss >> item) {
            items.push_back(item);
        }
        
        if (items.size() > 2 && items[2] == device_id) {
            return true;
        }
    }
    
    return false;
}

void Filesystem::_mount_and_resize(uint64_t pos) {
    if (resize_needs_mpoint && !is_mounted()) {
        auto mount = temp_mount();
        _resize(pos);
    } else {
        _resize(pos);
    }

    // measure size again
    read_superblock();
    assert(fssize() == pos);
}

uint64_t Filesystem::grow_nonrec(uint64_t upper_bound) {
    uint64_t newsize = align(upper_bound, block_size);
    assert(fssize() <= newsize);
    if (fssize() == newsize) {
        return newsize;
    }
    _mount_and_resize(newsize);
    return newsize;
}

uint64_t Filesystem::fssize() {
    if (sb_size_in_bytes) {
        assert(size_bytes % block_size == 0);
        return size_bytes;
    } else {
        return block_size * block_count;
    }
}

std::string Filesystem::fslabel() {
    std::vector<std::string> cmd = {"blkid", "-o", "value", "-s", "LABEL", "--", device.devpath};
    std::string result;
    
    FILE* pipe = popen((cmd[0] + " " + cmd[1] + " " + cmd[2] + " " + cmd[3] + " " + 
                        cmd[4] + " " + cmd[5] + " " + cmd[6]).c_str(), "r");
    if (!pipe) {
        return "";
    }
    
    char buffer[128];
    if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result = buffer;
        if (!result.empty() && result.back() == '\n') {
            result.pop_back();
        }
    }
    
    pclose(pipe);
    return result;
}

std::string Filesystem::fsuuid() {
    std::vector<std::string> cmd = {"blkid", "-o", "value", "-s", "UUID", "--", device.devpath};
    std::string result;
    
    FILE* pipe = popen((cmd[0] + " " + cmd[1] + " " + cmd[2] + " " + cmd[3] + " " + 
                        cmd[4] + " " + cmd[5] + " " + cmd[6]).c_str(), "r");
    if (!pipe) {
        return "";
    }
    
    char buffer[128];
    if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result = buffer;
        if (!result.empty() && result.back() == '\n') {
            result.pop_back();
        }
    }
    
    pclose(pipe);
    return result;
}

// XFS implementation
XFS::XFS(BlockDevice device) : Filesystem(device) {
    resize_needs_mpoint = true;
    vfstype = vfstype_str;
}

void XFS::read_superblock() {
    block_size = 0;
    block_count = 0;

    FILE* pipe = popen(("xfs_db -c 'sb 0' -c 'p dblocks blocksize' -- " + device.devpath).c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("Failed to execute xfs_db");
    }

    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        std::string line(buffer);
        if (line.find("dblocks =") == 0) {
            block_count = std::stoull(aftersep(line, "="));
        } else if (line.find("blocksize =") == 0) {
            block_size = std::stoull(aftersep(line, "="));
        }
    }

    pclose(pipe);
    assert(block_size != 0);
}

void XFS::_resize(uint64_t target_size) {
    assert(target_size % block_size == 0);
    uint64_t target_blocks = target_size / block_size;
    
    std::vector<std::string> cmd = {
        "xfs_growfs", "-D", std::to_string(target_blocks),
        "--", device.devpath
    };
    
    quiet_call(cmd);
}

// NilFS implementation
NilFS::NilFS(BlockDevice device) : Filesystem(device) {
    sb_size_in_bytes = true;
    resize_needs_mpoint = true;
    vfstype = vfstype_str;
}

void NilFS::read_superblock() {
    block_size = 0;
    size_bytes = 0;

    FILE* pipe = popen(("nilfs-tune -l -- " + device.devpath).c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("Failed to execute nilfs-tune");
    }

    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        std::string line(buffer);
        if (line.find("Block size:") == 0) {
            block_size = std::stoull(aftersep(line, ":"));
        } else if (line.find("Device size:") == 0) {
            size_bytes = std::stoull(aftersep(line, ":"));
        }
    }

    pclose(pipe);
    assert(block_size != 0);
}

void NilFS::_resize(uint64_t target_size) {
    assert(target_size % block_size == 0);
    
    std::vector<std::string> cmd = {
        "nilfs-resize", "--yes", "--",
        device.devpath, std::to_string(target_size)
    };
    
    quiet_call(cmd);
}

// BtrFS implementation
BtrFS::BtrFS(BlockDevice device) : Filesystem(device) {
    sb_size_in_bytes = true;
    // We'll get the mpoint ourselves
    resize_needs_mpoint = false;
    vfstype = vfstype_str;
}

void BtrFS::read_superblock() {
    block_size = 0;
    size_bytes = 0;
    devid = 0;

    FILE* pipe = popen(("btrfs-show-super -- " + device.devpath).c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("Failed to execute btrfs-show-super");
    }

    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        std::string line(buffer);
        if (starts_with_word(line, "dev_item.devid")) {
            devid = std::stoull(line.substr(line.find_first_of(" \t") + 1));
        } else if (starts_with_word(line, "sectorsize")) {
            block_size = std::stoull(line.substr(line.find_first_of(" \t") + 1));
        } else if (starts_with_word(line, "dev_item.total_bytes")) {
            size_bytes = std::stoull(line.substr(line.find_first_of(" \t") + 1));
        }
    }

    pclose(pipe);
    assert(block_size != 0);
}

void BtrFS::_resize(uint64_t target_size) {
    assert(target_size % block_size == 0);
    
    // XXX The device is unavailable (EBUSY)
    // immediately after unmounting.
    // Bug introduced in Linux 3.0, fixed in 3.9.
    // Tracked down by Eric Sandeen in
    // http://comments.gmane.org/gmane.comp.file-systems.btrfs/23987
    auto mount = temp_mount();
    
    std::vector<std::string> cmd = {
        "btrfs", "filesystem", "resize",
        std::to_string(devid) + ":" + std::to_string(target_size),
        mount->path()
    };
    
    quiet_call(cmd);
}

// ReiserFS implementation
ReiserFS::ReiserFS(BlockDevice device) : Filesystem(device) {
    vfstype = vfstype_str;
}

void ReiserFS::read_superblock() {
    block_size = 0;
    block_count = 0;

    FILE* pipe = popen(("reiserfstune -- " + device.devpath).c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("Failed to execute reiserfstune");
    }

    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        std::string line(buffer);
        if (line.find("Blocksize:") == 0) {
            block_size = std::stoull(aftersep(line, ":"));
        } else if (line.find("Count of blocks on the device:") == 0) {
            block_count = std::stoull(aftersep(line, ":"));
        }
    }

    pclose(pipe);
    assert(block_size != 0);
}

void ReiserFS::_resize(uint64_t target_size) {
    assert(target_size % block_size == 0);
    
    std::vector<std::string> cmd = {
        "resize_reiserfs", "-q", "-s", std::to_string(target_size),
        "--", device.devpath
    };
    
    quiet_call(cmd);
}

// ExtFS implementation
ExtFS::ExtFS(BlockDevice device) : Filesystem(device) {
    vfstype = vfstype_str;
}

void ExtFS::read_superblock() {
    block_size = 0;
    block_count = 0;
    state = "";
    mount_tm = 0;
    check_tm = 0;

    FILE* pipe = popen(("tune2fs -l -- " + device.devpath).c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("Failed to execute tune2fs");
    }

    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        std::string line(buffer);
        if (line.find("Block size:") == 0) {
            block_size = std::stoull(aftersep(line, ":"));
        } else if (line.find("Block count:") == 0) {
            block_count = std::stoull(aftersep(line, ":"));
        } else if (line.find("Filesystem state:") == 0) {
            state = aftersep(line, ":");
            // Trim leading whitespace
            state.erase(0, state.find_first_not_of(" \t"));
        } else if (line.find("Last mount time:") == 0) {
            std::string date = aftersep(line, ":");
            // Trim leading whitespace
            date.erase(0, date.find_first_not_of(" \t"));
            
            if (date == "n/a") {
                mount_tm = 0;
            } else {
                struct tm tm = {};
                strptime(date.c_str(), "%a %b %d %H:%M:%S %Y", &tm);
                mount_tm = mktime(&tm);
            }
        } else if (line.find("Last checked:") == 0) {
            std::string date = aftersep(line, ":");
            // Trim leading whitespace
            date.erase(0, date.find_first_not_of(" \t"));
            
            struct tm tm = {};
            strptime(date.c_str(), "%a %b %d %H:%M:%S %Y", &tm);
            check_tm = mktime(&tm);
        }
    }

    pclose(pipe);
    assert(block_size != 0);
}

void ExtFS::_resize(uint64_t target_size) {
    uint64_t block_count = target_size / block_size;
    assert(target_size % block_size == 0);

    // resize2fs requires that the filesystem was checked
    if (!is_mounted() && (state != "clean" || check_tm < mount_tm)) {
        std::cout << "Checking the filesystem before resizing it" << std::endl;
        // Can't use the -n flag, it is strictly read-only and won't
        // update check_tm in the superblock
        // XXX Without either of -n -p -y, e2fsck will require a
        // terminal on stdin
        std::vector<std::string> check_cmd = {
            "e2fsck", "-f", "--", device.devpath
        };
        quiet_call(check_cmd);
        check_tm = mount_tm;
    }
    
    std::vector<std::string> resize_cmd = {
        "resize2fs", "--", device.devpath, std::to_string(block_count)
    };
    
    quiet_call(resize_cmd);
}

// Swap implementation
Swap::Swap(BlockDevice device) : Filesystem(device) {
    vfstype = vfstype_str;
}

bool Swap::is_mounted() {
    // parse /proc/swaps, see tab_parse.c
    throw std::runtime_error("Not implemented");
}

void Swap::read_superblock() {
    // No need to do anything, UUID and LABEL are already
    // exposed through blkid
    int dev_fd = device.open_excl();
    auto [big_endian_val, version_val, last_page] = __read_sb(dev_fd);
    close(dev_fd);
    
    block_size = 4096;
    block_count = last_page + 1;
    big_endian = big_endian_val;
    version = version_val;
}

std::tuple<bool, uint32_t, uint32_t> Swap::__read_sb(int dev_fd) {
    // Assume 4k pages, bail otherwise
    // XXX The SB checks should be done before calling the constructor
    char magic_buf[10];
    if (pread(dev_fd, magic_buf, 10, 4096 - 10) != 10) {
        throw std::runtime_error("Failed to read swap magic");
    }
    
    if (std::string(magic_buf, 10) != "SWAPSPACE2") {
        // Might be suspend data
        std::map<std::string, std::string> kwargs;
        kwargs["magic"] = std::string(magic_buf, 10);
        throw UnsupportedSuperblock(device.devpath, kwargs);
    }
    
    uint32_t version, last_page;
    char version_buf[8];
    if (pread(dev_fd, version_buf, 8, 1024) != 8) {
        throw std::runtime_error("Failed to read swap version");
    }
    
    // Try big endian first
    version = (static_cast<uint32_t>(version_buf[0]) << 24) |
              (static_cast<uint32_t>(version_buf[1]) << 16) |
              (static_cast<uint32_t>(version_buf[2]) << 8) |
              static_cast<uint32_t>(version_buf[3]);
    
    last_page = (static_cast<uint32_t>(version_buf[4]) << 24) |
                (static_cast<uint32_t>(version_buf[5]) << 16) |
                (static_cast<uint32_t>(version_buf[6]) << 8) |
                static_cast<uint32_t>(version_buf[7]);
    
    bool big_endian = true;
    
    if (version != 1) {
        uint32_t version0 = version;
        // Try little endian
        version = static_cast<uint32_t>(version_buf[3]) |
                 (static_cast<uint32_t>(version_buf[2]) << 8) |
                 (static_cast<uint32_t>(version_buf[1]) << 16) |
                 (static_cast<uint32_t>(version_buf[0]) << 24);
        
        last_page = static_cast<uint32_t>(version_buf[7]) |
                   (static_cast<uint32_t>(version_buf[6]) << 8) |
                   (static_cast<uint32_t>(version_buf[5]) << 16) |
                   (static_cast<uint32_t>(version_buf[4]) << 24);
        
        big_endian = false;
        
        if (version != 1) {
            std::map<std::string, std::string> kwargs;
            kwargs["version"] = std::to_string(std::min(version, version0));
            throw UnsupportedSuperblock(device.devpath, kwargs);
        }
    }
    
    if (!last_page) {
        std::map<std::string, std::string> kwargs;
        kwargs["last_page"] = "0";
        throw UnsupportedSuperblock(device.devpath, kwargs);
    }
    
    return {big_endian, version, last_page};
}

void Swap::_resize(uint64_t target_size) {
    // using mkswap+swaplabel like GParted would drop some metadata
    char buf[8];
    
    if (big_endian) {
        buf[0] = (version >> 24) & 0xFF;
        buf[1] = (version >> 16) & 0xFF;
        buf[2] = (version >> 8) & 0xFF;
        buf[3] = version & 0xFF;
        
        uint32_t last_page = target_size / block_size - 1;
        buf[4] = (last_page >> 24) & 0xFF;
        buf[5] = (last_page >> 16) & 0xFF;
        buf[6] = (last_page >> 8) & 0xFF;
        buf[7] = last_page & 0xFF;
    } else {
        buf[3] = (version >> 24) & 0xFF;
        buf[2] = (version >> 16) & 0xFF;
        buf[1] = (version >> 8) & 0xFF;
        buf[0] = version & 0xFF;
        
        uint32_t last_page = target_size / block_size - 1;
        buf[7] = (last_page >> 24) & 0xFF;
        buf[6] = (last_page >> 16) & 0xFF;
        buf[5] = (last_page >> 8) & 0xFF;
        buf[4] = last_page & 0xFF;
    }
    
    int dev_fd = device.open_excl();
    if (pwrite(dev_fd, buf, 8, 1024) != 8) {
        close(dev_fd);
        throw std::runtime_error("Failed to write swap header");
    }
    close(dev_fd);
}

} // namespace blocks
