#ifndef LVM_OPERATIONS_H
#define LVM_OPERATIONS_H

#include "blocks_types.h"
#include "block_device.h"
#include "block_stack.h"
#include "synthetic_device.h"
#include <memory>
#include <string>
#include <vector>

namespace blocks {
    struct CommandArgs {
        std::string command;
        std::string device;
        std::string vgname;
        std::string join;
        bool debug = false;
        bool maintboot = false;
        bool resize_device = false;
        uint64_t newsize = 0;
    };
class Augeas {
public:
    Augeas(const std::string& loadpath, const std::string& root, int flags);
    
    int get_int(const std::string& key);
    void set_int(const std::string& key, int val);
    void incr(const std::string& key, int by = 1);
    void decr(const std::string& key);
    
    std::string get(const std::string& key);
    void set(const std::string& key, const std::string& val);
    
    void defvar(const std::string& name, const std::string& expr);
    void insert(const std::string& path, const std::string& label, bool before = true);
    void remove(const std::string& path);
    void rename(const std::string& src, const std::string& dst);
    
    void text_store(const std::string& lens, const std::string& name, const std::string& path);
    void text_retrieve(const std::string& lens, const std::string& src, const std::string& dest, const std::string& output);
};

// Rotate the augeas representation of LVM metadata
void rotate_aug(Augeas& aug, bool forward, uint64_t size);

// Rotate a logical volume by a single PE
void rotate_lv(BlockDevice& device, uint64_t size, bool debug, bool forward);

// Convert a device to LVM
int cmd_to_lvm(const struct CommandArgs& args);

} // namespace blocks

#endif // LVM_OPERATIONS_H
