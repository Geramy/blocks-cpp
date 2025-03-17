#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include "blocks_types.h"
#include "block_device.h"
#include <string>
#include <functional>
#include <memory>
#include <ctime>

namespace blocks {

class Filesystem : public BlockData {
public:
    Filesystem(BlockDevice device);
    virtual ~Filesystem() = default;

    bool resize_needs_mpoint = false;
    bool sb_size_in_bytes = false;
    
    uint64_t reserve_end_area_nonrec(uint64_t pos);
    
    class TempMount {
    public:
        TempMount(const std::string& devpath, const std::string& vfstype);
        ~TempMount();
        std::string path() const { return mpoint; }
        
    private:
        std::string mpoint;
        std::string devpath;
    };

    std::unique_ptr<TempMount> temp_mount();
    virtual bool is_mounted();
    
    void _mount_and_resize(uint64_t pos);
    virtual void _resize(uint64_t pos) = 0;
    
    uint64_t grow_nonrec(uint64_t upper_bound);
    
    uint64_t fssize();
    std::string fslabel();
    std::string fsuuid();
    
    virtual void read_superblock() = 0;
    virtual bool can_shrink() const = 0;
    
    uint64_t block_size;
    uint64_t block_count;
    uint64_t size_bytes;
    std::string vfstype;
};

class XFS : public Filesystem {
public:
    XFS(BlockDevice device);
    
    bool can_shrink() const override { return false; }
    void read_superblock() override;
    void _resize(uint64_t target_size) override;
    
    static constexpr const char* vfstype_str = "xfs";
};

class NilFS : public Filesystem {
public:
    NilFS(BlockDevice device);
    
    bool can_shrink() const override { return true; }
    void read_superblock() override;
    void _resize(uint64_t target_size) override;
    
    static constexpr const char* vfstype_str = "nilfs2";
};

class BtrFS : public Filesystem {
public:
    BtrFS(BlockDevice device);
    
    bool can_shrink() const override { return true; }
    void read_superblock() override;
    void _resize(uint64_t target_size) override;
    
    static constexpr const char* vfstype_str = "btrfs";
    
    uint64_t devid;
};

class ReiserFS : public Filesystem {
public:
    ReiserFS(BlockDevice device);
    
    bool can_shrink() const override { return true; }
    void read_superblock() override;
    void _resize(uint64_t target_size) override;
    
    static constexpr const char* vfstype_str = "reiserfs";
};

class ExtFS : public Filesystem {
public:
    ExtFS(BlockDevice device);
    
    bool can_shrink() const override { return true; }
    void read_superblock() override;
    void _resize(uint64_t target_size) override;
    
    static constexpr const char* vfstype_str = "ext4"; // Covers ext2/3/4
    
    std::string state;
    std::time_t mount_tm;
    std::time_t check_tm;
};

class Swap : public Filesystem {
public:
    Swap(BlockDevice device);
    
    bool can_shrink() const override { return true; }
    void read_superblock() override;
    void _resize(uint64_t target_size) override;
    
    static constexpr const char* vfstype_str = "swap";
    
    bool is_mounted() override;
    
private:
    std::tuple<bool, uint32_t, uint32_t> __read_sb(int dev_fd);
    
    bool big_endian;
    uint32_t version;
};

} // namespace blocks

#endif // FILESYSTEM_H
