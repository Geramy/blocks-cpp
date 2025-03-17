#ifndef SYNTHETIC_DEVICE_H
#define SYNTHETIC_DEVICE_H

#include "blocks_types.h"
#include "block_device.h"
#include <memory>
#include <string>
#include <functional>
#include <vector>

namespace blocks {

class SyntheticDevice : public BlockDevice {
public:
    SyntheticDevice(const std::string& devpath);
    
    void copy_to_physical(
        int dev_fd, 
        uint64_t shift_by = 0, 
        uint64_t reserved_area = 0, 
        bool other_device = false
    );
    
    std::vector<uint8_t> data;
    uint64_t writable_hdr_size;
    uint64_t rz_size;
    uint64_t writable_end_size;
};

class SyntheticDeviceContext {
public:
    SyntheticDeviceContext(uint64_t writable_hdr_size, uint64_t rz_size, uint64_t writable_end_size = 0);
    ~SyntheticDeviceContext();
    
    SyntheticDevice* operator->();
    SyntheticDevice& operator*();
    
private:
    std::unique_ptr<SyntheticDevice> device;
    std::string temp_file_path;
    std::string lo_dev_path;
    std::string rozeros_devname;
    std::string synth_devname;
    std::string synth_devpath;
    std::function<void()> exit_callback;
    
    void cleanup();
};

// Factory function to create a synthetic device with the specified parameters
std::unique_ptr<SyntheticDeviceContext> synth_device(
    uint64_t writable_hdr_size, 
    uint64_t rz_size, 
    uint64_t writable_end_size = 0
);

} // namespace blocks

#endif // SYNTHETIC_DEVICE_H
