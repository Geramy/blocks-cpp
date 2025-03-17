#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <array>
#include <stdexcept>
#include <regex>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <cassert>
#include <cstring>
#include <fstream>
#include <sstream>

// Constants
#define PE_SIZE (4 * 1024 * 1024)  // 4 MiB Physical Extent size

// Utility function to execute shell commands and capture output
std::string execCommand(const std::string& cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) {
        throw std::runtime_error("popen() failed for command: " + cmd);
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    if (pclose(pipe.release()) != 0) {
        throw std::runtime_error("Command failed: " + cmd);
    }
    return result;
}

// **BlockDevice Class**: Represents a block device and provides methods to query its properties
class BlockDevice {
public:
    explicit BlockDevice(const std::string& devpath) : devpath_(devpath) {
        if (access(devpath.c_str(), F_OK) != 0) {
            throw std::runtime_error("Device does not exist: " + devpath);
        }
    }

    // Open device with exclusive access
    int openExcl() const {
        return open(devpath_.c_str(), O_SYNC | O_RDWR | O_EXCL);
    }

    // Get filesystem type using blkid
    std::string getSuperblockType() const {
        std::string cmd = "blkid -p -o value -s TYPE -- " + devpath_;
        std::string result = execCommand(cmd);
        return result.empty() ? "" : result.substr(0, result.find('\n'));
    }

    // Get device size in bytes
    long long getSize() const {
        std::string cmd = "blockdev --getsize64 -- " + devpath_;
        std::string result = execCommand(cmd);
        long long size = std::stoll(result);
        if (size % 512 != 0) {
            throw std::runtime_error("Size not sector-aligned: " + devpath_);
        }
        return size;
    }

    // Check if device is already an LVM physical volume
    bool isLVM() const {
        std::string cmd = "lvm pvs --noheadings -o pv_name -- " + devpath_;
        std::string result = execCommand(cmd);
        return !result.empty() && result.find(devpath_) != std::string::npos;
    }

    const std::string& getDevpath() const { return devpath_; }

private:
    std::string devpath_;
};

// **Filesystem Base Class**: Abstract class for filesystem operations
class Filesystem {
public:
    explicit Filesystem(const BlockDevice& device) : device_(device) {}
    virtual ~Filesystem() = default;

    virtual void readSuperblock() = 0;  // Read filesystem metadata
    virtual void resize(long long newSize) = 0;  // Resize filesystem
    virtual bool canShrink() const = 0;  // Check if shrinking is supported
    virtual long long getSize() const = 0;  // Get current filesystem size
    const BlockDevice& device_;
    const BlockDevice& getDevice() const {
        return device_;
    }
};

// **ExtFS Class**: Implementation for Ext2/Ext3/Ext4 filesystems
class ExtFS : public Filesystem {
public:
    explicit ExtFS(const BlockDevice& device)
            : Filesystem(device), blockSize_(0), blockCount_(0) {}

    void readSuperblock() override {
        std::string cmd = "tune2fs -l -- " + device_.getDevpath();
        std::string output = execCommand(cmd);
        std::regex blockSizeRe("Block size:\\s+(\\d+)");
        std::regex blockCountRe("Block count:\\s+(\\d+)");
        std::smatch match;

        if (std::regex_search(output, match, blockSizeRe)) {
            blockSize_ = std::stoll(match[1]);
        }
        if (std::regex_search(output, match, blockCountRe)) {
            blockCount_ = std::stoll(match[1]);
        }
        if (blockSize_ == 0 || blockCount_ == 0) {
            throw std::runtime_error("Failed to read ExtFS superblock: " + device_.getDevpath());
        }
    }

    void resize(long long newSize) override {
        long long newBlocks = newSize / blockSize_;
        if (newSize % blockSize_ != 0) {
            throw std::invalid_argument("New size must be block-aligned");
        }
        std::string cmd = "resize2fs -- " + device_.getDevpath() + " " + std::to_string(newBlocks);
        execCommand(cmd);
        readSuperblock();  // Update size after resize
    }

    bool canShrink() const override { return true; }

    long long getSize() const override { return blockSize_ * blockCount_; }

private:
    long long blockSize_;
    long long blockCount_;
};

// **BlockStack Class**: Manages filesystem stack and resizing
class BlockStack {
public:
    explicit BlockStack(std::unique_ptr<Filesystem> fs) : fs_(std::move(fs)) {}

    void readSuperblocks() {
        fs_->readSuperblock();
    }

    void stackResize(long long newSize, bool shrink) {
        if (shrink) {
            stackReserveEndArea(newSize);
        } else {
            stackGrow(newSize);
        }
    }

    long long totalDataSize() const {
        return fs_->getSize();
    }

    const BlockDevice& getDevice() const { return fs_->device_; }

private:
    void stackReserveEndArea(long long pos) {
        long long currentSize = fs_->getSize();
        if (currentSize > pos) {
            if (!fs_->canShrink()) {
                throw std::runtime_error("Cannot shrink filesystem: " + fs_->getDevice().getDevpath());
            }
            std::cout << "Shrinking filesystem by " << (currentSize - pos) << " bytes\n";
            fs_->resize(pos);
        } else {
            std::cout << "Filesystem leaves enough room, no shrink needed\n";
        }
    }

    void stackGrow(long long newSize) {
        if (fs_->getSize() < newSize) {
            fs_->resize(newSize);
        }
    }

    std::unique_ptr<Filesystem> fs_;
};

// **Helper Function**: Create BlockStack based on filesystem type
std::unique_ptr<BlockStack> getBlockStack(const BlockDevice& device) {
    std::string sbType = device.getSuperblockType();
    if (sbType == "ext2" || sbType == "ext3" || sbType == "ext4") {
        return std::make_unique<BlockStack>(std::make_unique<ExtFS>(device));
    } else {
        throw std::runtime_error("Unsupported superblock type: " + sbType);
    }
}

// **Main LVM Conversion Function**
void cmdToLVM(const std::string& devicePath, const std::string& vgName = "") {
    // Initialize block device
    BlockDevice device(devicePath);

    // Check if already LVM
    if (device.isLVM()) {
        throw std::runtime_error("Device is already an LVM physical volume: " + devicePath);
    }

    // Calculate sizes
    long long devSize = device.getSize();
    long long peCount = devSize / PE_SIZE - 1;  // Reserve one PE for metadata
    long long peNewPos = peCount * PE_SIZE;

    // Set up filesystem stack
    auto blockStack = getBlockStack(device);
    blockStack->readSuperblocks();
    blockStack->stackResize(peNewPos, true);

    // Copy first block to new position
    int devFd = device.openExcl();
    if (devFd < 0) {
        throw std::runtime_error("Failed to open device exclusively: " + devicePath);
    }

    std::vector<char> peData(PE_SIZE);
    if (pread(devFd, peData.data(), PE_SIZE, 0) != PE_SIZE) {
        close(devFd);
        throw std::runtime_error("Failed to read initial PE data from " + devicePath);
    }
    if (pwrite(devFd, peData.data(), PE_SIZE, peNewPos) != PE_SIZE) {
        close(devFd);
        throw std::runtime_error("Failed to write PE data to position " + std::to_string(peNewPos));
    }
    std::cout << "Copied " << PE_SIZE << " bytes to position " << peNewPos << "\n";

    // Generate LVM configuration (placeholders for UUIDs)
    std::string pvUuid = "generated-pv-uuid";  // In practice, use a UUID library
    std::string vgUuid = "generated-vg-uuid";
    std::string lvUuid = "generated-lv-uuid";
    std::string lvName = "lv1";  // Simplified; could derive from label
    std::string effectiveVgName = vgName.empty() ? "vg." + devicePath.substr(5) : vgName;

    // Create LVM config file content
    std::stringstream cfg;
    cfg << "contents = \"Text Format Volume Group\"\n";
    cfg << "version = 1\n";
    cfg << effectiveVgName << " {\n";
    cfg << "    id = \"" << vgUuid << "\"\n";
    cfg << "    seqno = 0\n";
    cfg << "    status = [\"RESIZEABLE\", \"READ\", \"WRITE\"]\n";
    cfg << "    extent_size = 8192\n";  // 4 MiB in 512-byte sectors
    cfg << "    max_lv = 0\n";
    cfg << "    max_pv = 0\n";
    cfg << "    physical_volumes {\n";
    cfg << "        pv0 {\n";
    cfg << "            id = \"" << pvUuid << "\"\n";
    cfg << "            status = [\"ALLOCATABLE\"]\n";
    cfg << "            pe_start = 8192\n";  // Offset in sectors
    cfg << "            pe_count = " << peCount << "\n";
    cfg << "        }\n";
    cfg << "    }\n";
    cfg << "    logical_volumes {\n";
    cfg << "        " << lvName << " {\n";
    cfg << "            id = \"" << lvUuid << "\"\n";
    cfg << "            status = [\"READ\", \"WRITE\", \"VISIBLE\"]\n";
    cfg << "            segment_count = 2\n";
    cfg << "            segment1 {\n";
    cfg << "                start_extent = 0\n";
    cfg << "                extent_count = 1\n";
    cfg << "                type = \"striped\"\n";
    cfg << "                stripe_count = 1\n";
    cfg << "                stripes = [\"pv0\", " << (peCount - 1) << "]\n";
    cfg << "            }\n";
    cfg << "            segment2 {\n";
    cfg << "                start_extent = 1\n";
    cfg << "                extent_count = " << (peCount - 1) << "\n";
    cfg << "                type = \"striped\"\n";
    cfg << "                stripe_count = 1\n";
    cfg << "                stripes = [\"pv0\", 0]\n";
    cfg << "            }\n";
    cfg << "        }\n";
    cfg << "    }\n";
    cfg << "}\n";

    // Write config to temporary file
    std::string cfgFile = "/tmp/vg.cfg";
    std::ofstream out(cfgFile);
    if (!out) {
        close(devFd);
        throw std::runtime_error("Failed to create config file: " + cfgFile);
    }
    out << cfg.str();
    out.close();

    // Set up LVM
    try {
        execCommand("lvm pvcreate --restorefile " + cfgFile + " --uuid " + pvUuid + " --zero y -- " + devicePath);
        execCommand("lvm vgcfgrestore --file " + cfgFile + " -- " + effectiveVgName);
        std::cout << "LVM conversion successful! VG: " << effectiveVgName << ", LV: " << lvName << "\n";
    } catch (const std::exception& e) {
        close(devFd);
        unlink(cfgFile.c_str());
        throw;
    }

    // Cleanup
    close(devFd);
    unlink(cfgFile.c_str());
}

// **Main Function**: Entry point with command-line argument parsing
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <device> [--vg-name <name>]\n";
        return 1;
    }

    std::string devicePath = argv[1];
    std::string vgName = "";
    if (argc >= 4 && std::string(argv[2]) == "--vg-name") {
        vgName = argv[3];
    }

    try {
        cmdToLVM(devicePath, vgName);
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 2;
    }

    return 0;
}