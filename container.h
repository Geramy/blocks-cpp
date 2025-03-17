#ifndef CONTAINER_H
#define CONTAINER_H

#include "blocks_types.h"
#include "block_device.h"
#include <memory>
#include <string>
#include <optional>

namespace blocks {

    class SimpleContainer : public BlockData {
    public:
        explicit SimpleContainer(BlockDevice device);  // Added explicit
        virtual ~SimpleContainer() = default;

        // A single block device that wraps a single block device
        // (luks is one, but not lvm, lvm is m2m)

        uint64_t offset = 0;  // Consider protected with getter if encapsulation is desired
    };

    class BCacheBacking : public SimpleContainer {
    public:
        explicit BCacheBacking(BlockDevice device);  // Added explicit
        virtual ~BCacheBacking() = default;

        void read_superblock();

        bool is_backing();
        bool is_activated();
        BlockDevice cached_device();
        void deactivate();
        uint64_t grow_nonrec(uint64_t upper_bound);

    private:
        std::optional<int> version;
        std::unordered_map<std::string, BlockDevice> memoized_devices;
        memoized_property<BlockDevice, BCacheBacking> _cached_device;  // Fixed template parameters
    };

    class LUKS : public SimpleContainer {
    public:
        explicit LUKS(BlockDevice device);  // Added explicit
        virtual ~LUKS() = default;

        // pycryptsetup isn't used because:
        //     it isn't in PyPI, or in Debian or Ubuntu
        //     it isn't Python 3
        //     it's incomplete (resize not included)

        void activate(const std::string& dmname);
        void deactivate();
        BlockDevice snoop_activated();
        BlockDevice cleartext_device();

        void read_superblock();
        void read_superblock_ll(int fd);
        void shift_sb(int fd, uint64_t shift_by);

        uint64_t grow_nonrec(uint64_t upper_bound);
        uint64_t reserve_end_area_nonrec(uint64_t pos);
        uint64_t sb_end = 0;

    private:
        bool _superblock_read = false;

        std::unordered_map<std::string, BlockDevice> memoized_devices;
        memoized_property<BlockDevice, LUKS> _cleartext_device;  // Fixed template parameters
    };

} // namespace blocks

#endif // CONTAINER_H