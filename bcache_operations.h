#ifndef BCACHE_OPERATIONS_H
#define BCACHE_OPERATIONS_H

#include "blocks_types.h"
#include "block_device.h"
#include "block_stack.h"
#include "synthetic_device.h"
#include "container.h"
#include <memory>
#include <string>

namespace blocks {

// Create a bcache superblock with the specified parameters
std::unique_ptr<SyntheticDevice> make_bcache_sb(uint64_t bsb_size, uint64_t data_size, const std::string& join);

// Convert an LVM logical volume to bcache
int lv_to_bcache(BlockDevice device, bool debug, ProgressListener& progress, const std::string& join);

// Convert a LUKS volume to bcache
int luks_to_bcache(BlockDevice device, bool debug, ProgressListener& progress, const std::string& join);

// Convert a partition to bcache
int part_to_bcache(BlockDevice device, bool debug, ProgressListener& progress, const std::string& join);

// Command handler for bcache conversion
int cmd_to_bcache(int argc, char* argv[]);

} // namespace blocks

#endif // BCACHE_OPERATIONS_H
