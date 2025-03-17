#include "resize_operations.h"
#include <iostream>
#include <regex>
#include <complex>

namespace blocks {

int cmd_resize(const std::string& device_path, uint64_t newsize, bool resize_device, bool debug) {
    BlockDevice device(device_path);
    CLIProgressHandler progress;

    BlockStack block_stack = get_block_stack(device, progress);

    int64_t device_delta = static_cast<int64_t>(newsize) - static_cast<int64_t>(device.size());

    if (device_delta > 0 && resize_device) {
        device.dev_resize(newsize, false);
        // May have been rounded up for the sake of partition alignment
        // LVM rounds up as well (and its LV metadata uses PE units)
        newsize = device.size();
    }

    block_stack.read_superblocks();
    assert(block_stack.total_data_size() <= device.size());
    int64_t data_delta = static_cast<int64_t>(newsize) - static_cast<int64_t>(block_stack.total_data_size());
    block_stack.stack_resize(newsize, data_delta < 0, progress);

    if (device_delta < 0 && resize_device) {
        uint64_t tds = block_stack.total_data_size();
        // LVM should be able to reload in-use devices,
        // but the kernel's partition handling can't.
        if (device.is_partition()) {
            block_stack.deactivate();
        }
        device.dev_resize(tds, true);
    }

    return 0;
}

int cmd_resize(const ResizeArgs& args) {
    return cmd_resize(args.device, args.newsize, args.resize_device, args.debug);
}

} // namespace blocks
