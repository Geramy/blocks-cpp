#include "container.h"
#include <iostream>
#include <regex>
#include <fstream>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

namespace blocks {

SimpleContainer::SimpleContainer(BlockDevice device) : BlockData(device) {}

BCacheBacking::BCacheBacking(BlockDevice device) : SimpleContainer(device), 
    _cached_device(&BCacheBacking::cached_device, "cached_device", "Cached device") {}

void BCacheBacking::read_superblock() {
    offset = 0;
    version = std::nullopt;

    std::vector<std::string> cmd = {"bcache-super-show", "--", device.devpath};
    
    FILE* pipe = popen((cmd[0] + " " + cmd[1] + " " + cmd[2]).c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("Failed to execute bcache-super-show");
    }
    
    char buffer[128];
    std::string line;
    
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        line = buffer;
        if (starts_with_word(line, "sb.version")) {
            version = std::stoi(line.substr(line.find_first_of("0123456789")));
        } else if (starts_with_word(line, "dev.data.first_sector")) {
            std::string value = line.substr(line.find_first_of("0123456789"));
            offset = std::stoull(value) * 512;
        }
    }
    
    pclose(pipe);
    
    if (!offset) {
        throw std::runtime_error("Failed to determine bcache offset");
    }
}

bool BCacheBacking::is_backing() {
    // Whitelist versions, just in case newer backing devices
    // are too different
    return version.has_value() && (version.value() == 1 || version.value() == 4);
}

bool BCacheBacking::is_activated() {
    return std::filesystem::exists(device.sysfspath() + "/bcache");
}

BlockDevice BCacheBacking::cached_device() {
    if (!is_activated()) {
        // XXX How synchronous is this?
        std::ofstream register_file("/sys/fs/bcache/register");
        if (!register_file) {
            throw std::runtime_error("Failed to open bcache register file");
        }
        register_file << device.devpath << std::endl;
        register_file.close();
    }
    return BlockDevice(devpath_from_sysdir(device.sysfspath() + "/bcache/dev"));
}

void BCacheBacking::deactivate() {
    std::ofstream stop_file(device.sysfspath() + "/bcache/stop");
    if (!stop_file) {
        throw std::runtime_error("Failed to open bcache stop file");
    }
    // XXX Asynchronous
    stop_file << "stop" << std::endl;
    stop_file.close();
    
    if (is_activated()) {
        throw std::runtime_error("Failed to deactivate bcache device");
    }
    
    _cached_device.reset(this, memoized_devices);
}

uint64_t BCacheBacking::grow_nonrec(uint64_t upper_bound) {
    if (upper_bound != device.size()) {
        throw std::runtime_error("Not implemented: bcache resize to size other than device size");
    }
    
    if (!is_activated()) {
        // Nothing to do, bcache will pick up the size on activation
        return upper_bound;
    }
    
    std::ofstream resize_file(device.sysfspath() + "/bcache/resize");
    if (!resize_file) {
        throw std::runtime_error("Failed to open bcache resize file");
    }
    // XXX How synchronous is this?
    resize_file << "max" << std::endl;
    resize_file.close();
    
    BlockDevice cached = cached_device();
    cached.reset_size();
    
    if (cached.size() + offset != upper_bound) {
        throw std::runtime_error("Bcache resize failed: cached device size + offset != upper_bound");
    }
    
    return upper_bound;
}

LUKS::LUKS(BlockDevice device) : SimpleContainer(device), 
    _cleartext_device(&LUKS::cleartext_device, "cleartext_device", "Cleartext device") {}

void LUKS::activate(const std::string& dmname) {
    std::vector<std::string> cmd = {"cryptsetup", "luksOpen", "--", device.devpath, dmname};
    quiet_call(cmd);
}

void LUKS::deactivate() {
    while (true) {
        BlockDevice dev = snoop_activated();
        if (dev.devpath.empty()) {
            break;
        }
        std::vector<std::string> cmd = {"cryptsetup", "remove", "--", dev.devpath};
        quiet_call(cmd);
    }
    _cleartext_device.reset(this, memoized_devices);
}

    BlockDevice LUKS::snoop_activated() {
        if (!_superblock_read) {
            read_superblock();
        }
        for (auto& hld : device.iter_holders()) {
            std::string table = hld.dm_table();
            std::string plainsize, cipher, major, minor, offset_str, options;
            // Provide arguments for all 6 capture groups
            if (dm_crypt_re.FullMatch(table, &plainsize, &cipher, &major, &minor, &offset_str, &options)) {
                uint64_t table_offset = std::stoull(offset_str);
                if (table_offset == bytes_to_sector(offset)) {
                    return hld;
                }
            }
        }
        return BlockDevice("");
    }

BlockDevice LUKS::cleartext_device() {
    // If the device is already activated we won't have
    // to prompt for a passphrase.
    BlockDevice dev = snoop_activated();
    if (dev.devpath.empty()) {
        uuid_t uuid;
        uuid_generate(uuid);
        char uuid_str[37];
        uuid_unparse(uuid, uuid_str);
        
        std::string dmname = "cleartext-" + std::string(uuid_str);
        activate(dmname);
        dev = BlockDevice("/dev/mapper/" + dmname);
    }
    return dev;
}

void LUKS::read_superblock() {
    // read the cyphertext's luks superblock
    offset = 0;

    std::vector<std::string> cmd = {"cryptsetup", "luksDump", "--", device.devpath};
    
    FILE* pipe = popen((cmd[0] + " " + cmd[1] + " " + cmd[2] + " " + cmd[3]).c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("Failed to execute cryptsetup luksDump");
    }
    
    char buffer[128];
    std::string line;
    
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        line = buffer;
        if (line.find("Payload offset:") == 0) {
            std::string value = aftersep(line, ":");
            offset = std::stoull(value) * 512;
        }
    }
    
    pclose(pipe);
    
    if (!offset) {
        throw std::runtime_error("Failed to determine LUKS offset");
    }
    
    _superblock_read = true;
}

void LUKS::read_superblock_ll(int fd) {
    // Low-level
    // https://cryptsetup.googlecode.com/git/docs/on-disk-format.pdf

    sb_end = 0;
    
    char buffer[8];
    if (pread(fd, buffer, 8, 0) != 8) {
        throw std::runtime_error("Failed to read LUKS header magic");
    }
    
    std::string magic(buffer, 6);
    uint16_t version;
    std::memcpy(&version, buffer + 6, 2);
    version = be16toh(version);  // Convert from big-endian
    
    if (magic != "LUKS\xBA\xBE") {
        throw std::runtime_error("Invalid LUKS magic");
    }
    if (version != 1) {
        throw std::runtime_error("Unsupported LUKS version");
    }

    if (pread(fd, buffer, 8, 104) != 8) {
        throw std::runtime_error("Failed to read LUKS payload info");
    }
    
    uint32_t payload_start_sectors, key_bytes;
    std::memcpy(&payload_start_sectors, buffer, 4);
    std::memcpy(&key_bytes, buffer + 4, 4);
    payload_start_sectors = be32toh(payload_start_sectors);  // Convert from big-endian
    key_bytes = be32toh(key_bytes);  // Convert from big-endian
    
    uint64_t sb_end_tmp = 592;

    for (int key_slot = 0; key_slot < 8; key_slot++) {
        if (pread(fd, buffer, 8, 208 + 48 * key_slot + 40) != 8) {
            throw std::runtime_error("Failed to read LUKS key slot info");
        }
        
        uint32_t key_offset, key_stripes;
        std::memcpy(&key_offset, buffer, 4);
        std::memcpy(&key_stripes, buffer + 4, 4);
        key_offset = be32toh(key_offset);  // Convert from big-endian
        key_stripes = be32toh(key_stripes);  // Convert from big-endian
        
        if (key_stripes != 4000) {
            throw std::runtime_error("Unexpected LUKS key stripes value");
        }
        
        uint64_t key_size = key_stripes * key_bytes;
        uint64_t key_end = key_offset * 512 + key_size;
        if (key_end > sb_end_tmp) {
            sb_end_tmp = key_end;
        }
    }

    uint64_t ll_offset = payload_start_sectors * 512;
    if (ll_offset != offset) {
        throw std::runtime_error("LUKS offset mismatch between high-level and low-level reads");
    }
    
    if (ll_offset < sb_end_tmp) {
        throw std::runtime_error("LUKS payload offset is less than superblock end");
    }
    
    sb_end = sb_end_tmp;
}

void LUKS::shift_sb(int fd, uint64_t shift_by) {
    if (shift_by == 0 || shift_by % 512 != 0 || offset % 512 != 0) {
        throw std::runtime_error("Invalid LUKS shift parameters");
    }
    
    if (sb_end + shift_by > offset) {
        throw std::runtime_error("Not enough space to shift LUKS superblock");
    }

    // Read the superblock
    std::vector<char> sb(sb_end);
    if (pread(fd, sb.data(), sb_end, 0) != static_cast<ssize_t>(sb_end)) {
        throw std::runtime_error("Failed to read LUKS superblock for shifting");
    }

    // Edit the sb
    uint32_t offset_sectors = offset / 512;
    uint32_t new_offset_sectors = offset_sectors - shift_by / 512;
    
    // Convert to big-endian
    new_offset_sectors = htobe32(new_offset_sectors);
    
    // Update the offset in the superblock
    std::memcpy(sb.data() + 104, &new_offset_sectors, 4);

    // Create a buffer with zeros for the shift
    std::vector<char> zeros(shift_by, 0);
    
    // Combine zeros and shifted superblock
    std::vector<char> combined(shift_by + sb_end);
    std::memcpy(combined.data(), zeros.data(), shift_by);
    std::memcpy(combined.data() + shift_by, sb.data(), sb_end);

    // Write the shifted, edited superblock
    ssize_t wr_len = pwrite(fd, combined.data(), combined.size(), 0);
    if (wr_len != static_cast<ssize_t>(combined.size())) {
        throw std::runtime_error("Failed to write shifted LUKS superblock");
    }

    // Wipe the results of read_superblock_ll
    // Keep self.offset for now
    sb_end = 0;
}

uint64_t LUKS::grow_nonrec(uint64_t upper_bound) {
    return reserve_end_area_nonrec(upper_bound);
}

uint64_t LUKS::reserve_end_area_nonrec(uint64_t pos) {
    // cryptsetup uses the inner size
    uint64_t inner_size = pos - offset;
    uint64_t sectors = bytes_to_sector(inner_size);

    // pycryptsetup is useless, no resize support
    // otoh, size doesn't appear in the superblock,
    // and updating the dm table is only useful if
    // we want to do some fsck before deactivating
    std::vector<std::string> cmd = {
        "cryptsetup", "resize", "--size=" + std::to_string(sectors),
        "--", cleartext_device().devpath
    };
    quiet_call(cmd);
    
    BlockDevice activated = snoop_activated();
    if (!activated.devpath.empty()) {
        activated.reset_size();
        if (activated.size() != inner_size) {
            throw std::runtime_error("LUKS resize failed: cleartext device size != inner_size");
        }
    }
    
    return pos;
}

} // namespace blocks
