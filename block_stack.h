#ifndef BLOCK_STACK_H
#define BLOCK_STACK_H

#include "blocks_types.h"
#include "block_device.h"
#include "filesystem.h"
#include "container.h"
#include <vector>
#include <memory>

namespace blocks {

class BlockStack {
public:
    BlockStack(std::vector<std::shared_ptr<BlockData>> stack);

    std::vector<std::shared_ptr<BlockData>> wrappers();
    uint64_t overhead();
    std::shared_ptr<BlockData> topmost();
    std::string fsuuid();
    std::string fslabel();
    
    std::vector<std::pair<uint64_t, std::shared_ptr<BlockData>>> iter_pos(uint64_t pos);
    
    uint64_t total_data_size();
    
    void stack_resize(uint64_t pos, bool shrink, ProgressListener& progress);
    void stack_grow(uint64_t newsize, ProgressListener& progress);
    void stack_reserve_end_area(uint64_t pos, ProgressListener& progress);
    
    void read_superblocks();
    void deactivate();
    
private:
    std::vector<std::shared_ptr<BlockData>> stack;
};

BlockStack get_block_stack(BlockDevice device, ProgressListener& progress);

} // namespace blocks

#endif // BLOCK_STACK_H
