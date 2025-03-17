#include "block_stack.h"
#include <algorithm>
#include <iostream>

namespace blocks {

    BlockStack::BlockStack(std::vector<std::shared_ptr<BlockData>> stack)
            : stack(std::move(stack)) {
    }

    std::vector<std::shared_ptr<BlockData>> BlockStack::wrappers() {
        if (stack.empty()) return {};
        return std::vector<std::shared_ptr<BlockData>>(stack.begin(), stack.end() - 1);
    }

    uint64_t BlockStack::overhead() {
        uint64_t total_offset = 0;
        for (auto& wrapper : wrappers()) {
            if (auto container = std::dynamic_pointer_cast<SimpleContainer>(wrapper)) {
                total_offset += container->offset;
            }
        }
        return total_offset;
    }

    std::shared_ptr<BlockData> BlockStack::topmost() {
        if (stack.empty()) return nullptr;
        return stack.back();
    }

    std::string BlockStack::fsuuid() {
        if (auto fs = std::dynamic_pointer_cast<Filesystem>(topmost())) {
            return fs->fsuuid();
        }
        return "";
    }

    std::string BlockStack::fslabel() {
        if (auto fs = std::dynamic_pointer_cast<Filesystem>(topmost())) {
            return fs->fslabel();
        }
        return "";
    }

    std::vector<std::pair<uint64_t, std::shared_ptr<BlockData>>> BlockStack::iter_pos(uint64_t pos) {
        std::vector<std::pair<uint64_t, std::shared_ptr<BlockData>>> result;

        for (auto& block_data : wrappers()) {
            result.emplace_back(pos, block_data);
            if (auto container = std::dynamic_pointer_cast<SimpleContainer>(block_data)) {
                pos -= container->offset;
            }
        }

        if (auto top = topmost()) {
            result.emplace_back(pos, top);
        }
        return result;
    }

    uint64_t BlockStack::total_data_size() {
        uint64_t fs_size = 0;
        if (auto fs = std::dynamic_pointer_cast<Filesystem>(topmost())) {
            fs_size = fs->fssize();
        }
        return fs_size + overhead();
    }

    void BlockStack::stack_resize(uint64_t pos, bool shrink, ProgressListener& progress) {
        if (shrink) {
            stack_reserve_end_area(pos, progress);
        } else {
            stack_grow(pos, progress);
        }
    }

    void BlockStack::stack_grow(uint64_t newsize, ProgressListener& progress) {
        uint64_t current_size = newsize;

        for (auto& block_data : wrappers()) {
            if (auto container = std::dynamic_pointer_cast<SimpleContainer>(block_data)) {
                if (auto luks = std::dynamic_pointer_cast<LUKS>(block_data)) {
                    current_size = luks->grow_nonrec(current_size);
                } else if (auto bcache = std::dynamic_pointer_cast<BCacheBacking>(block_data)) {
                    current_size = bcache->grow_nonrec(current_size);
                }
                current_size -= container->offset;
            }
        }

        if (auto fs = std::dynamic_pointer_cast<Filesystem>(topmost())) {
            fs->grow_nonrec(current_size);
        }
    }

    void BlockStack::stack_reserve_end_area(uint64_t pos, ProgressListener& progress) {
        auto fs = std::dynamic_pointer_cast<Filesystem>(topmost());
        if (!fs) {
            progress.bail("Topmost layer is not a filesystem", std::runtime_error("Invalid stack"));
            return;
        }

        uint64_t inner_pos = align(pos - overhead(), fs->block_size);
        uint64_t shrink_size = fs->fssize() - inner_pos;
        std::string fstype = fs->device.superblock_type();

        if (fs->fssize() > inner_pos) {
            if (fs->can_shrink()) {
                progress.notify("Will shrink the filesystem (" + fstype + ") by " +
                                std::to_string(shrink_size) + " bytes");
            } else {
                progress.bail("Can't shrink filesystem (" + fstype + "), but need another " +
                              std::to_string(shrink_size) + " bytes at the end",
                              CantShrink());
            }
        } else {
            progress.notify("The filesystem (" + fstype + ") leaves enough room, no need to shrink it");
        }

        // While there may be no need to shrink the topmost fs,
        // the wrapper stack needs to be updated for the new size
        auto positions = iter_pos(pos);
        std::reverse(positions.begin(), positions.end());

        for (auto& [inner_pos, block_data] : positions) {
            if (auto fs_ptr = std::dynamic_pointer_cast<Filesystem>(block_data)) {
                fs_ptr->reserve_end_area_nonrec(inner_pos);
            } else if (auto luks = std::dynamic_pointer_cast<LUKS>(block_data)) {
                luks->reserve_end_area_nonrec(inner_pos);
            }
        }
    }

    void BlockStack::read_superblocks() {
        for (auto& wrapper : wrappers()) {
            if (auto luks = std::dynamic_pointer_cast<LUKS>(wrapper)) {
                luks->read_superblock();
            } else if (auto bcache = std::dynamic_pointer_cast<BCacheBacking>(wrapper)) {
                bcache->read_superblock();
            }
        }

        if (auto fs = std::dynamic_pointer_cast<Filesystem>(topmost())) {
            fs->read_superblock();
        }
    }

    void BlockStack::deactivate() {
        // Deactivate in reverse order
        for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
            if (auto luks = std::dynamic_pointer_cast<LUKS>(*it)) {
                luks->deactivate();
            } else if (auto bcache = std::dynamic_pointer_cast<BCacheBacking>(*it)) {
                bcache->deactivate();
            }
        }

        // Salt the earth, our devpaths are obsolete now
        stack.clear();
    }

    BlockStack get_block_stack(BlockDevice device, ProgressListener& progress) {
        std::vector<std::shared_ptr<BlockData>> stack;

        while (true) {
            std::string superblock_type = device.superblock_type();

            if (superblock_type == "crypto_LUKS") {
                auto wrapper = std::make_shared<LUKS>(device);
                stack.push_back(wrapper);
                device = wrapper->cleartext_device();
                continue;
            } else if (device.has_bcache_superblock()) {
                auto wrapper = std::make_shared<BCacheBacking>(device);
                wrapper->read_superblock();
                if (!wrapper->is_backing()) {
                    progress.bail("BCache device isn't a backing device",
                                  UnsupportedSuperblock(device.devpath));
                }
                stack.push_back(wrapper);
                device = wrapper->cached_device();
                continue;
            }

            std::shared_ptr<Filesystem> fs;

            if (superblock_type == "ext2" || superblock_type == "ext3" || superblock_type == "ext4") {
                fs = std::make_shared<ExtFS>(device);
            } else if (superblock_type == "reiserfs") {
                fs = std::make_shared<ReiserFS>(device);
            } else if (superblock_type == "btrfs") {
                fs = std::make_shared<BtrFS>(device);
            } else if (superblock_type == "nilfs2") {
                fs = std::make_shared<NilFS>(device);
            } else if (superblock_type == "xfs") {
                fs = std::make_shared<XFS>(device);
            } else if (superblock_type == "swap") {
                fs = std::make_shared<Swap>(device);
            } else {
                if (superblock_type.empty()) {
                    progress.bail("Unrecognised superblock", UnsupportedSuperblock(device.devpath));
                } else {
                    progress.bail("Unsupported superblock type: " + superblock_type,
                                  UnsupportedSuperblock(device.devpath));
                }
            }

            stack.push_back(fs);
            break;  // Exit loop after adding filesystem
        }

        return BlockStack(stack);
    }

} // namespace blocks