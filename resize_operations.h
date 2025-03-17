#ifndef RESIZE_OPERATIONS_H
#define RESIZE_OPERATIONS_H

#include "blocks_types.h"
#include "block_device.h"
#include "block_stack.h"
#include <string>
#include <cstdint>

namespace blocks {

/**
 * Parse a size argument from a string with unit suffix
 * 
 * @param size String representation of size with optional unit suffix (bkmgtpe)
 * @return Size in bytes
 * @throws std::invalid_argument if the size format is invalid
 */
uint64_t parse_size_arg(const std::string& size);

/**
 * Resize a block device or filesystem
 * 
 * @param device Path to the device to resize
 * @param newsize New size in bytes
 * @param resize_device Whether to resize the device itself or just the contents
 * @param debug Enable debug output
 * @return Exit code (0 for success)
 */
int cmd_resize(const std::string& device, uint64_t newsize, bool resize_device, bool debug);

/**
 * Resize a block device or filesystem (argument struct version)
 * 
 * @param args Command line arguments structure
 * @return Exit code (0 for success)
 */
int cmd_resize(const struct ResizeArgs& args);

/**
 * Arguments for resize operation
 */
struct ResizeArgs {
    std::string device;
    uint64_t newsize;
    bool resize_device;
    bool debug;
};

} // namespace blocks

#endif // RESIZE_OPERATIONS_H
