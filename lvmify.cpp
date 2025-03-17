#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#include <CLI/CLI.hpp>

namespace fs = std::filesystem;

// Constants
const uint64_t LVM_PE_SIZE = 4 * 1024 * 1024; // 4MiB PE for vgmerge compatibility
const std::string ASCII_ALNUM_WHITELIST = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.";
const std::string BCACHE_MAGIC = std::string("\xc6\x85\x73\xf6\x4e\x1a\x45\xca\x82\x65\xf5\x7f\x48\xba\x6d\x81", 16);

// Regular expressions
const std::regex DM_CRYPT_RE("^0 (\\d+) crypt ([a-z0-9:-]+) 0+ 0 (\\d+):(\\d+) (\\d+)( [^\n]*)?\n$", std::regex::ECMAScript);
const std::regex SIZE_RE("^(\\d+)([bkmgtpe])?$", std::regex::icase);

// **Utility Functions**
uint64_t bytes_to_sector(uint64_t bytes) {
    assert(bytes % 512 == 0);
    return bytes / 512;
}

uint64_t intdiv_up(uint64_t num, uint64_t denom) {
    return (num - 1) / denom + 1;
}

uint64_t align_up(uint64_t size, uint64_t align) {
    return intdiv_up(size, align) * align;
}

uint64_t align(uint64_t size, uint64_t align) {
    return (size / align) * align;
}

std::string exec_command(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) throw std::runtime_error("popen failed: " + cmd);
    char buffer[128];
    std::string result;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    int status = pclose(pipe);
    if (status != 0) throw std::runtime_error("Command failed: " + cmd + " with status " + std::to_string(status));
    if (!result.empty() && result.back() == '\n') result.pop_back();
    return result;
}

void quiet_call(const std::string& cmd) {
    int status = system(cmd.c_str());
    if (status != 0) throw std::runtime_error("Command failed: " + cmd);
}

// **Exception Classes**
class UnsupportedSuperblock : public std::runtime_error {
public:
    std::string device;
    UnsupportedSuperblock(const std::string& dev, const std::string& msg)
            : std::runtime_error("UnsupportedSuperblock: device=" + dev + ", " + msg), device(dev) {}
};

class CantShrink : public std::runtime_error {
public:
    CantShrink() : std::runtime_error("CantShrink") {}
};

class OverlappingPartition : public std::runtime_error {
public:
    OverlappingPartition() : std::runtime_error("OverlappingPartition") {}
};

class MissingRequirement : public std::runtime_error {
public:
    MissingRequirement(const std::string& cmd) : std::runtime_error("MissingRequirement: " + cmd) {}
};

// **BlockDevice Class**
class BlockDevice {
public:
    explicit BlockDevice(const std::string& devpath) : devpath_(devpath) {
        if (!fs::exists(devpath)) throw std::runtime_error("Device does not exist: " + devpath);
    }

    static BlockDevice by_uuid(const std::string& uuid) {
        std::string devpath = exec_command("blkid -U " + uuid);
        return BlockDevice(devpath);
    }

    int open_excl() const {
        return open(devpath_.c_str(), O_SYNC | O_RDWR | O_EXCL);
    }

    std::string get_devpath() const { return devpath_; }

    std::string get_superblock_type() const {
        if (!superblock_type_.has_value()) {
            superblock_type_ = exec_command("blkid -p -o value -s TYPE -- " + devpath_);
            if (superblock_type_->empty()) superblock_type_ = "";
        }
        return superblock_type_.value();
    }

    uint64_t get_size() const {
        if (!size_.has_value()) {
            size_ = std::stoull(exec_command("blockdev --getsize64 " + devpath_));
            assert(size_.value() % 512 == 0);
        }
        return size_.value();
    }

    void reset_size() { size_.reset(); }

    std::string get_sysfspath() const {
        struct stat st;
        if (stat(devpath_.c_str(), &st) != 0 || !S_ISBLK(st.st_mode)) {
            throw std::runtime_error("Not a block device: " + devpath_);
        }
        return "/sys/dev/block/" + std::to_string(major(st.st_rdev)) + ":" + std::to_string(minor(st.st_rdev));
    }

    std::pair<int, int> get_devnum() const {
        struct stat st;
        if (stat(devpath_.c_str(), &st) != 0 || !S_ISBLK(st.st_mode)) {
            throw std::runtime_error("Not a block device: " + devpath_);
        }
        return {major(st.st_rdev), minor(st.st_rdev)};
    }

    std::vector<BlockDevice> iter_holders() const {
        std::vector<BlockDevice> holders;
        std::string holders_path = get_sysfspath() + "/holders";
        if (fs::exists(holders_path)) {
            for (const auto& entry : fs::directory_iterator(holders_path)) {
                holders.emplace_back("/dev/" + entry.path().filename().string());
            }
        }
        return holders;
    }

    bool is_dm() const {
        if (!is_dm_.has_value()) {
            is_dm_ = fs::exists(get_sysfspath() + "/dm");
        }
        return is_dm_.value();
    }

    std::string dm_table() const {
        return exec_command("dmsetup table -- " + devpath_);
    }

    void dm_deactivate() const {
        quiet_call("dmsetup remove -- " + devpath_);
    }

    bool is_partition() const {
        if (!is_partition_.has_value()) {
            std::string part_file = get_sysfspath() + "/partition";
            is_partition_ = (fs::exists(part_file) && std::ifstream(part_file).rdbuf()->in_avail() != 0);
        }
        return is_partition_.value();
    }

    void dev_resize(uint64_t newsize, bool shrink) {
        newsize = align_up(newsize, 512);
        if (is_partition()) {
            throw std::runtime_error("Partition resizing requires libparted, not implemented");
        } else {
            throw std::runtime_error("Only partitions can be resized in this simplified version");
        }
        reset_size();
    }

private:
    std::string devpath_;
    mutable std::optional<std::string> superblock_type_;
    mutable std::optional<uint64_t> size_;
    mutable std::optional<bool> is_dm_;
    mutable std::optional<bool> is_partition_;
};

// **BlockData and Derived Classes**
class BlockData {
public:
    explicit BlockData(BlockDevice* device) : device_(device) {}
    virtual ~BlockData() = default;

protected:
    BlockDevice* device_;
};

class Filesystem : public BlockData {
public:
    Filesystem(BlockDevice* device) : BlockData(device) {}
    virtual bool can_shrink() const = 0;
    virtual bool resize_needs_mpoint() const = 0;
    virtual void read_superblock() = 0;
    virtual void reserve_end_area_nonrec(uint64_t pos) = 0;
    virtual void grow_nonrec(uint64_t upper_bound) = 0;
    virtual uint64_t get_fssize() const = 0;
    virtual std::string get_vfstype() const = 0;

    std::string get_fslabel() const {
        if (!fslabel_.has_value()) {
            fslabel_ = exec_command("blkid -o value -s LABEL -- " + device_->get_devpath());
        }
        return fslabel_.value();
    }

    std::string get_fsuuid() const {
        if (!fsuuid_.has_value()) {
            fsuuid_ = exec_command("blkid -o value -s UUID -- " + device_->get_devpath());
        }
        return fsuuid_.value();
    }

protected:
    mutable std::optional<std::string> fslabel_;
    mutable std::optional<std::string> fsuuid_;
    uint32_t block_size_ = 0;
    uint64_t block_count_ = 0;

    void mount_and_resize(uint64_t pos) {
        if (resize_needs_mpoint() && !is_mounted()) {
            fs::path mpoint = fs::temp_directory_path() / ("privmnt" + std::to_string(rand()));
            fs::create_directory(mpoint);
            quiet_call("mount -t " + get_vfstype() + " -o noatime,noexec,nodev -- " + device_->get_devpath() + " " + mpoint.string());
            resize(pos);
            quiet_call("umount -- " + mpoint.string());
            fs::remove(mpoint);
        } else {
            resize(pos);
        }
        read_superblock();
        assert(get_fssize() == pos);
    }

    virtual void resize(uint64_t target_size) = 0;

    bool is_mounted() const {
        auto [major, minor] = device_->get_devnum();
        std::string dn = std::to_string(major) + ":" + std::to_string(minor);
        std::ifstream mounts("/proc/self/mountinfo");
        std::string line;
        while (std::getline(mounts, line)) {
            std::istringstream iss(line);
            std::string item;
            std::vector<std::string> items;
            while (iss >> item) items.push_back(item);
            if (items[2] == dn) return true;
        }
        return false;
    }
};

class ExtFS : public Filesystem {
public:
    ExtFS(BlockDevice* device) : Filesystem(device) {}

    bool can_shrink() const override { return true; }
    bool resize_needs_mpoint() const override { return false; }
    std::string get_vfstype() const override { return "ext4"; }

    void read_superblock() override {
        std::string output = exec_command("dumpe2fs -h " + device_->get_devpath());
        std::istringstream iss(output);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.find("Block count:") == 0) block_count_ = std::stoull(line.substr(13));
            else if (line.find("Block size:") == 0) block_size_ = std::stoul(line.substr(12));
        }
    }

    void reserve_end_area_nonrec(uint64_t pos) override {
        pos = align(pos, block_size_);
        if (get_fssize() <= pos) return;
        mount_and_resize(pos);
    }

    void grow_nonrec(uint64_t upper_bound) override {
        uint64_t newsize = align(upper_bound, block_size_);
        if (get_fssize() >= newsize) return;
        mount_and_resize(newsize);
    }

    uint64_t get_fssize() const override { return block_size_ * block_count_; }

private:
    void resize(uint64_t target_size) override {
        uint64_t target_blocks = target_size / block_size_;
        quiet_call("resize2fs " + device_->get_devpath() + " " + std::to_string(target_blocks));
    }
};

class LVM : public BlockData {
public:
    LVM(BlockDevice* device) : BlockData(device) {}

    void read_superblock() override {
        std::string output = exec_command("lvs --noheadings -o lv_size --units b -- " + device_->get_devpath());
        size_ = std::stoull(output);
    }

    void grow_nonrec(uint64_t upper_bound) override {
        uint64_t newsize = align(upper_bound, LVM_PE_SIZE);
        if (get_size() >= newsize) return;
        quiet_call("lvresize -L " + std::to_string(newsize) + "b -- " + device_->get_devpath());
        read_superblock();
    }

    void reserve_end_area_nonrec(uint64_t pos) override {
        pos = align(pos, LVM_PE_SIZE);
        if (get_size() <= pos) return;
        quiet_call("lvresize -L " + std::to_string(pos) + "b -- " + device_->get_devpath());
        read_superblock();
    }

    uint64_t get_size() const { return size_; }

private:
    uint64_t size_ = 0;
};

class Bcache : public BlockData {
public:
    Bcache(BlockDevice* device) : BlockData(device) {}

    void read_superblock() override {
        std::string output = exec_command("bcache-super-show " + device_->get_devpath());
        std::istringstream iss(output);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.find("sb.version") == 0 && line.find("cache") != std::string::npos) {
                is_cache_ = true;
            } else if (line.find("dev.size") == 0) {
                size_ = std::stoull(line.substr(10)) * 512; // Convert sectors to bytes
            }
        }
    }

    void register_cache() {
        quiet_call("make-bcache -C " + device_->get_devpath());
    }

    void register_backing(const std::string& cache_dev) {
        quiet_call("make-bcache -B " + device_->get_devpath() + " -C " + cache_dev);
    }

    BlockDevice get_bcache_device() {
        for (const auto& hld : device_->iter_holders()) {
            if (hld.get_superblock_type() == "bcache") return hld;
        }
        throw std::runtime_error("No bcache device found for " + device_->get_devpath());
    }

    uint64_t get_size() const { return size_; }

private:
    bool is_cache_ = false;
    uint64_t size_ = 0;
};

// **BlockStack Class**
class BlockStack {
public:
    explicit BlockStack(std::vector<std::unique_ptr<BlockData>> stack) : stack_(std::move(stack)) {}

    std::vector<BlockData*> get_wrappers() const {
        std::vector<BlockData*> wrappers;
        for (size_t i = 0; i < stack_.size() - 1; ++i) {
            wrappers.push_back(stack_[i].get());
        }
        return wrappers;
    }

    uint64_t get_overhead() const {
        uint64_t overhead = 0;
        for (auto* wrapper : get_wrappers()) {
            if (auto* lvm = dynamic_cast<LVM*>(wrapper)) {
                overhead += LVM_PE_SIZE - (lvm->get_size() % LVM_PE_SIZE); // PE alignment
            }
        }
        return overhead;
    }

    Filesystem* get_topmost() const {
        return dynamic_cast<Filesystem*>(stack_.back().get());
    }

    std::string get_fsuuid() const { return get_topmost()->get_fsuuid(); }
    std::string get_fslabel() const { return get_topmost()->get_fslabel(); }

    uint64_t get_total_data_size() const {
        return get_topmost()->get_fssize() + get_overhead();
    }

    void stack_resize(uint64_t pos, bool shrink, class ProgressListener& progress) {
        if (shrink) {
            stack_reserve_end_area(pos, progress);
        } else {
            stack_grow(pos, progress);
        }
    }

    void read_superblocks() {
        for (auto& data : stack_) {
            if (auto* fs = dynamic_cast<Filesystem*>(data.get())) fs->read_superblock();
            else if (auto* lvm = dynamic_cast<LVM*>(data.get())) lvm->read_superblock();
            else if (auto* bcache = dynamic_cast<Bcache*>(data.get())) bcache->read_superblock();
        }
    }

    void deactivate() {
        for (auto* wrapper : get_wrappers() | std::views::reverse) {
            if (auto* lvm = dynamic_cast<LVM*>(wrapper)) {
                quiet_call("lvremove -f " + lvm->device_->get_devpath());
            }
        }
        stack_.clear();
    }

private:
    std::vector<std::unique_ptr<BlockData>> stack_;

    void stack_grow(uint64_t newsize, ProgressListener& progress) {
        uint64_t size = newsize;
        for (auto* wrapper : get_wrappers()) {
            if (auto* lvm = dynamic_cast<LVM*>(wrapper)) {
                lvm->grow_nonrec(size);
                size -= LVM_PE_SIZE - (lvm->get_size() % LVM_PE_SIZE);
            }
        }
        get_topmost()->grow_nonrec(size);
    }

    void stack_reserve_end_area(uint64_t pos, ProgressListener& progress) {
        uint64_t inner_pos = align(pos - get_overhead(), get_topmost()->block_size_);
        uint64_t shrink_size = get_topmost()->get_fssize() - inner_pos;
        std::string fstype = stack_[0]->device_->get_superblock_type();

        if (get_topmost()->get_fssize() > inner_pos) {
            if (get_topmost()->can_shrink()) {
                progress.notify("Will shrink the filesystem (" + fstype + ") by " + std::to_string(shrink_size) + " bytes");
            } else {
                progress.bail("Can't shrink filesystem (" + fstype + "), but need another " + std::to_string(shrink_size) + " bytes at the end", CantShrink());
            }
        } else {
            progress.notify("The filesystem (" + fstype + ") leaves enough room, no need to shrink it");
        }

        for (auto [inner_pos_it, block_data] : iter_pos(pos)) {
            if (auto* fs = dynamic_cast<Filesystem*>(block_data)) {
                fs->reserve_end_area_nonrec(inner_pos_it);
            } else if (auto* lvm = dynamic_cast<LVM*>(block_data)) {
                lvm->reserve_end_area_nonrec(inner_pos_it);
            }
        }
    }

    std::vector<std::pair<uint64_t, BlockData*>> iter_pos(uint64_t pos) const {
        std::vector<std::pair<uint64_t, BlockData*>> result;
        uint64_t current_pos = pos;
        for (auto* wrapper : get_wrappers()) {
            result.emplace_back(current_pos, wrapper);
            if (auto* lvm = dynamic_cast<LVM*>(wrapper)) {
                current_pos -= LVM_PE_SIZE - (lvm->get_size() % LVM_PE_SIZE);
            }
        }
        result.emplace_back(current_pos, get_topmost());
        return result;
    }
};

// **ProgressListener**
class ProgressListener {
public:
    virtual void notify(const std::string& msg) = 0;
    virtual void bail(const std::string& msg, const std::exception& err) = 0;
    virtual ~ProgressListener() = default;
};

class CLIProgressHandler : public ProgressListener {
public:
    void notify(const std::string& msg) override {
        std::cout << msg << std::endl;
    }

    void bail(const std::string& msg, const std::exception& err) override {
        std::cerr << msg << std::endl;
        throw err;
    }
};

// **BlockStack Factory Function**
BlockStack get_block_stack(BlockDevice& device, ProgressListener& progress) {
    std::vector<std::unique_ptr<BlockData>> stack;
    BlockDevice* current_device = &device;

    while (true) {
        std::string sb_type = current_device->get_superblock_type();
        if (sb_type == "LVM2_member") {
            stack.push_back(std::make_unique<LVM>(current_device));
            break; // LVM is typically the top layer here
        } else if (sb_type == "bcache") {
            stack.push_back(std::make_unique<Bcache>(current_device));
            break;
        } else if (sb_type == "ext4") {
            stack.push_back(std::make_unique<ExtFS>(current_device));
            break;
        } else {
            progress.bail(sb_type.empty() ? "Unrecognised superblock" : "Unsupported superblock type: " + sb_type,
                          UnsupportedSuperblock(current_device->get_devpath(), ""));
        }
    }
    return BlockStack(std::move(stack));
}

// **Command Functions**
void cmd_to_lvm(const CLI::App& app, const std::string& device_path, const std::string& vg_name, const std::string& lv_name) {
    BlockDevice device(device_path);
    CLIProgressHandler progress;
    BlockStack block_stack = get_block_stack(device, progress);

    block_stack.read_superblocks();
    Filesystem* fs = block_stack.get_topmost();
    if (fs->get_vfstype() != "ext4") {
        progress.bail("Only ext4 filesystems can be converted to LVM in this implementation", std::runtime_error("Unsupported filesystem"));
    }

    uint64_t orig_size = fs->get_fssize();
    progress.notify("Converting " + device_path + " to LVM with VG=" + vg_name + ", LV=" + lv_name);

    // Shrink filesystem to make room for LVM metadata
    uint64_t metadata_size = align_up(1 * 1024 * 1024, LVM_PE_SIZE); // 1MiB for LVM metadata
    block_stack.stack_resize(orig_size - metadata_size, true, progress);

    // Create physical volume
    quiet_call("pvcreate " + device_path);
    quiet_call("vgcreate " + vg_name + " " + device_path);
    quiet_call("lvcreate -L " + std::to_string(orig_size) + "b -n " + lv_name + " " + vg_name);

    // Copy data to LVM
    BlockDevice lv_device("/dev/" + vg_name + "/" + lv_name);
    std::unique_ptr<ExtFS> new_fs = std::make_unique<ExtFS>(&lv_device);
    new_fs->read_superblock();

    int src_fd = device.open_excl();
    int dst_fd = lv_device.open_excl();
    char buffer[4096];
    uint64_t offset = 0;
    while (offset < orig_size) {
        ssize_t bytes_read = pread(src_fd, buffer, sizeof(buffer), offset);
        if (bytes_read <= 0) break;
        pwrite(dst_fd, buffer, bytes_read, offset);
        offset += bytes_read;
    }
    close(src_fd);
    close(dst_fd);

    progress.notify("Data moved to LVM logical volume " + lv_device.get_devpath());
}

void cmd_to_bcache(const CLI::App& app, const std::string& backing_dev, const std::string& cache_dev) {
    BlockDevice backing(backing_dev);
    BlockDevice cache(cache_dev);
    CLIProgressHandler progress;

    progress.notify("Setting up bcache with backing=" + backing_dev + ", cache=" + cache_dev);
    Bcache cache_bcache(&cache);
    cache_bcache.register_cache();
    Bcache backing_bcache(&backing);
    backing_bcache.register_backing(cache_dev);

    BlockDevice bcache_dev = backing_bcache.get_bcache_device();
    progress.notify("Bcache device created: " + bcache_dev.get_devpath());
}

void cmd_resize(const CLI::App& app, const std::string& device_path, uint64_t newsize, bool resize_device) {
    BlockDevice device(device_path);
    CLIProgressHandler progress;

    BlockStack block_stack = get_block_stack(device, progress);
    int64_t device_delta = newsize - device.get_size();

    if (device_delta > 0 && resize_device) {
        device.dev_resize(newsize, false);
        newsize = device.get_size();
    }

    block_stack.read_superblocks();
    assert(block_stack.get_total_data_size() <= device.get_size());
    int64_t data_delta = newsize - block_stack.get_total_data_size();
    block_stack.stack_resize(newsize, data_delta < 0, progress);

    if (device_delta < 0 && resize_device) {
        uint64_t tds = block_stack.get_total_data_size();
        if (device.is_partition()) {
            block_stack.deactivate();
        }
        device.dev_resize(tds, true);
    }
}

void cmd_rotate(const CLI::App& app, const std::string& device_path, const std::string& new_device_path) {
    BlockDevice old_device(device_path);
    BlockDevice new_device(new_device_path);
    CLIProgressHandler progress;

    BlockStack old_stack = get_block_stack(old_device, progress);
    old_stack.read_superblocks();
    Filesystem* fs = old_stack.get_topmost();
    uint64_t data_size = old_stack.get_total_data_size();

    progress.notify("Rotating data from " + device_path + " to " + new_device_path);

    // Ensure new device is large enough
    if (new_device.get_size() < data_size) {
        progress.bail("New device is too small: " + std::to_string(new_device.get_size()) + " < " + std::to_string(data_size),
                      std::runtime_error("Insufficient space"));
    }

    // Copy data
    int src_fd = old_device.open_excl();
    int dst_fd = new_device.open_excl();
    char buffer[4096];
    uint64_t offset = 0;
    while (offset < data_size) {
        ssize_t bytes_read = pread(src_fd, buffer, sizeof(buffer), offset);
        if (bytes_read <= 0) break;
        pwrite(dst_fd, buffer, bytes_read, offset);
        offset += bytes_read;
    }
    close(src_fd);
    close(dst_fd);

    // Update filesystem on new device
    BlockStack new_stack = get_block_stack(new_device, progress);
    new_stack.read_superblocks();
    progress.notify("Rotation complete to " + new_device_path);
}

uint64_t parse_size_arg(const std::string& size) {
    std::smatch match;
    if (!std::regex_match(size, match, SIZE_RE)) {
        throw std::runtime_error("Size must be a decimal integer and an optional unit suffix (bkmgtpe)");
    }
    uint64_t val = std::stoull(match[1]);
    std::string unit = match[2].str();
    if (unit.empty()) unit = "b";
    static const std::string units = "bkmgtpe";
    size_t pos = units.find(unit[0]);
    if (pos == std::string::npos) throw std::runtime_error("Invalid unit: " + unit);
    return val * (1ULL << (10 * pos));
}

// **Main Function**
int main() {
    assert(true); // Ensure assertions are enabled

    CLI::App app{"Block device management tool"};
    app.require_subcommand();

    bool debug = false;
    app.add_flag("--debug", debug, "Enable debug mode");

    // Subcommand: to-lvm
    auto to_lvm = app.add_subcommand("to-lvm", "Convert device to LVM");
    std::string device_to_lvm, vg_name, lv_name;
    to_lvm->add_option("device", device_to_lvm, "Device path")->required();
    to_lvm->add_option("--vg-name", vg_name, "Volume group name")->default_val("vg0");
    to_lvm->add_option("--lv-name", lv_name, "Logical volume name")->default_val("lv0");

    // Subcommand: to-bcache
    auto to_bcache = app.add_subcommand("to-bcache", "Convert devices to bcache");
    std::string backing_dev, cache_dev;
    to_bcache->add_option("--backing", backing_dev, "Backing device path")->required();
    to_bcache->add_option("--cache", cache_dev, "Cache device path")->required();

    // Subcommand: resize
    auto resize = app.add_subcommand("resize", "Resize device or contents");
    std::string device_resize, newsize_str;
    bool resize_device = false;
    resize->add_option("device", device_resize, "Device path")->required();
    resize->add_flag("--resize-device", resize_device, "Resize the device itself");
    resize->add_option("newsize", newsize_str, "New size (e.g., 10g)")->required();

    // Subcommand: rotate
    auto rotate = app.add_subcommand("rotate", "Rotate data to a new device");
    std::string device_rotate, new_device;
    rotate->add_option("device", device_rotate, "Source device path")->required();
    rotate->add_option("new-device", new_device, "Destination device path")->required();

    CLI11_PARSE(app);

    try {
        if (to_lvm->parsed()) {
            cmd_to_lvm(*to_lvm, device_to_lvm, vg_name, lv_name);
        } else if (to_bcache->parsed()) {
            cmd_to_bcache(*to_bcache, backing_dev, cache_dev);
        } else if (resize->parsed()) {
            uint64_t newsize = parse_size_arg(newsize_str);
            cmd_resize(*resize, device_resize, newsize, resize_device);
        } else if (rotate->parsed()) {
            cmd_rotate(*rotate, device_rotate, new_device);
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 2;
    }
    return 0;
}