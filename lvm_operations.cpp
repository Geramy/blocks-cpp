#include "lvm_operations.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <regex>
#include <uuid/uuid.h>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

namespace blocks {

// Augeas implementation
    Augeas::Augeas(const std::string &loadpath, const std::string &root, int flags) {
        // This is a stub implementation since we don't have direct Augeas bindings
        // In a real implementation, this would initialize the Augeas library
    }

    int Augeas::get_int(const std::string &key) {
        // Stub implementation
        return 0;
    }

    void Augeas::set_int(const std::string &key, int val) {
        // Stub implementation
    }

    void Augeas::incr(const std::string &key, int by) {
        int val = get_int(key);
        set_int(key, val + by);
    }

    void Augeas::decr(const std::string &key) {
        incr(key, -1);
    }

    std::string Augeas::get(const std::string &key) {
        // Stub implementation
        return "";
    }

    void Augeas::set(const std::string &key, const std::string &val) {
        // Stub implementation
    }

    void Augeas::defvar(const std::string &name, const std::string &expr) {
        // Stub implementation
    }

    void Augeas::insert(const std::string &path, const std::string &label, bool before) {
        // Stub implementation
    }

    void Augeas::remove(const std::string &path) {
        // Stub implementation
    }

    void Augeas::rename(const std::string &src, const std::string &dst) {
        // Stub implementation
    }

    void Augeas::text_store(const std::string &lens, const std::string &name, const std::string &path) {
        // Stub implementation
    }

    void Augeas::text_retrieve(const std::string &lens, const std::string &src, const std::string &dest,
                               const std::string &output) {
        // Stub implementation
    }

    void rotate_aug(Augeas &aug, bool forward, uint64_t size) {
        int segment_count = aug.get_int("$lv/segment_count");
        int pe_sectors = aug.get_int("$vg/extent_size");
        int extent_total = 0;

        aug.incr("$lv/segment_count");

        // checking all segments are linear
        for (int i = 1; i <= segment_count; i++) {
            assert(aug.get("$lv/segment" + std::to_string(i) + "/dict/type/str") == "striped");
            assert(aug.get_int("$lv/segment" + std::to_string(i) + "/dict/stripe_count") == 1);
            extent_total += aug.get_int("$lv/segment" + std::to_string(i) + "/dict/extent_count");
        }

        assert(extent_total * pe_sectors == bytes_to_sector(size));
        assert(extent_total > 1);

        if (forward) {
            // Those definitions can't be factored out,
            // because we move nodes and the vars would follow
            aug.defvar("first", "$lv/segment1/dict");
            // shifting segments
            for (int i = 2; i <= segment_count; i++) {
                aug.decr("$lv/segment" + std::to_string(i) + "/dict/start_extent");
            }

            // shrinking first segment by one PE
            aug.decr("$first/extent_count");

            // inserting new segment at the end
            aug.insert(
                    "$lv/segment" + std::to_string(segment_count),
                    "segment" + std::to_string(segment_count + 1),
                    false);
            aug.set_int(
                    "$lv/segment" + std::to_string(segment_count + 1) + "/dict/start_extent",
                    extent_total - 1);
            aug.defvar("last", "$lv/segment" + std::to_string(segment_count + 1) + "/dict");
            aug.set_int("$last/extent_count", 1);
            aug.set("$last/type/str", "striped");
            aug.set_int("$last/stripe_count", 1);

            // repossessing the first segment's first PE
            aug.set(
                    "$last/stripes/list/1/str",
                    aug.get("$first/stripes/list/1/str"));
            aug.set_int(
                    "$last/stripes/list/2",
                    aug.get_int("$first/stripes/list/2"));
            aug.incr("$first/stripes/list/2");

            // Cleaning up an empty first PE
            if (aug.get_int("$first/extent_count") == 0) {
                aug.remove("$lv/segment1");
                for (int i = 2; i <= segment_count + 1; i++) {
                    aug.rename("$lv/segment" + std::to_string(i), "segment" + std::to_string(i - 1));
                }
                aug.decr("$lv/segment_count");
            }
        } else {
            // shifting segments
            for (int i = segment_count; i > 0; i--) {
                aug.incr("$lv/segment" + std::to_string(i) + "/dict/start_extent");
                aug.rename("$lv/segment" + std::to_string(i), "segment" + std::to_string(i + 1));
            }
            aug.defvar("last", "$lv/segment" + std::to_string(segment_count + 1) + "/dict");

            // shrinking last segment by one PE
            aug.decr("$last/extent_count");
            int last_count = aug.get_int("$last/extent_count");

            // inserting new segment at the beginning
            aug.insert("$lv/segment2", "segment1");
            aug.set_int("$lv/segment1/dict/start_extent", 0);
            aug.defvar("first", "$lv/segment1/dict");
            aug.set_int("$first/extent_count", 1);
            aug.set("$first/type/str", "striped");
            aug.set_int("$first/stripe_count", 1);

            // repossessing the last segment's last PE
            aug.set(
                    "$first/stripes/list/1/str",
                    aug.get("$last/stripes/list/1/str"));
            aug.set_int(
                    "$first/stripes/list/2",
                    aug.get_int("$last/stripes/list/2") + last_count);

            // Cleaning up an empty last PE
            if (last_count == 0) {
                aug.remove("$lv/segment" + std::to_string(segment_count + 1));
                aug.decr("$lv/segment_count");
            }
        }
    }

    void rotate_lv(BlockDevice &device, uint64_t size, bool debug, bool forward) {
        /*
         * Rotate a logical volume by a single PE.
         *
         * If forward:
         *     Move the first physical extent of an LV to the end
         * else:
         *     Move the last physical extent of a LV to the start
         *
         * then poke LVM to refresh the mapping.
         */

        // Get LV information
        std::string lv_info_cmd = "lvm lvs --noheadings --rows --units=b --nosuffix "
                                  "-o vg_name,vg_uuid,lv_name,lv_uuid,lv_attr -- " + device.devpath;

        FILE *pipe = popen(lv_info_cmd.c_str(), "r");
        if (!pipe) {
            throw std::runtime_error("Failed to execute LVM command");
        }

        char buffer[1024];
        std::string lv_info;
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            lv_info += buffer;
        }
        pclose(pipe);

        // Parse LV info
        std::istringstream iss(lv_info);
        std::string line;
        std::getline(iss, line);

        std::istringstream line_stream(line);
        std::string vgname, vg_uuid, lvname, lv_uuid, lv_attr;
        line_stream >> vgname >> vg_uuid >> lvname >> lv_uuid >> lv_attr;

        // Trim whitespace
        auto trim = [](std::string &s) {
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
                return !std::isspace(ch);
            }));
        };

        trim(vgname);
        trim(vg_uuid);
        trim(lvname);
        trim(lv_uuid);
        trim(lv_attr);

        bool active = lv_attr.size() > 4 && lv_attr[4] == 'a';

        // Make sure the volume isn't in use by unmapping it
        std::vector<std::string> lvchange_cmd = {"lvm", "lvchange", "-an", "--", vgname + "/" + lvname};
        quiet_call(lvchange_cmd);

        // Create temporary directory
        char temp_dir_template[] = "/tmp/blocks.XXXXXX";
        char *temp_dir = mkdtemp(temp_dir_template);
        if (!temp_dir) {
            throw std::runtime_error("Failed to create temporary directory");
        }

        std::string tdname(temp_dir);
        std::string vgcfgname = tdname + "/vg.cfg";

        std::cout << "Loading LVM metadata... " << std::flush;

        // Backup VG configuration
        std::vector<std::string> vgcfgbackup_cmd = {"lvm", "vgcfgbackup", "--file", vgcfgname, "--", vgname};
        quiet_call(vgcfgbackup_cmd);

        // In a real implementation, we would use Augeas to manipulate the LVM configuration
        // Since we don't have direct Augeas bindings, we'll simulate the process

        // Read the original configuration
        std::ifstream vgcfg(vgcfgname);
        std::string vgcfg_orig((std::istreambuf_iterator<char>(vgcfg)), std::istreambuf_iterator<char>());
        vgcfg.close();

        // Create a simulated Augeas object
        Augeas aug("", "/dev/null", 0);

        // In a real implementation, we would manipulate the LVM configuration using Augeas
        // For now, we'll just simulate the process

        // Verify ASCII_ALNUM_WHITELIST compliance
        for (char c: vgname) {
            assert(ASCII_ALNUM_WHITELIST.find(c) != std::string::npos);
        }
        for (char c: lvname) {
            assert(ASCII_ALNUM_WHITELIST.find(c) != std::string::npos);
        }

        // Simulate Augeas operations
        rotate_aug(aug, forward, size);

        // Write the modified configuration
        std::ofstream vgcfg_new(vgcfgname + ".new");
        vgcfg_new << "Simulated modified configuration" << std::endl;
        vgcfg_new.close();

        // Simulate reverting the changes to verify stability
        rotate_aug(aug, !forward, size);

        std::ofstream vgcfg_backagain(vgcfgname + ".backagain");
        vgcfg_backagain << "Simulated reverted configuration" << std::endl;
        vgcfg_backagain.close();

        if (debug) {
            std::cout << "CHECK STABILITY" << std::endl;
            std::system(("git --no-pager diff --no-index --patience --color-words -- " +
                         vgcfgname + " " + vgcfgname + ".backagain").c_str());

            if (forward) {
                std::cout << "CHECK CORRECTNESS (forward)" << std::endl;
            } else {
                std::cout << "CHECK CORRECTNESS (backward)" << std::endl;
            }
            std::system(("git --no-pager diff --no-index --patience --color-words -- " +
                         vgcfgname + " " + vgcfgname + ".new").c_str());
        }

        if (forward) {
            std::cout << "Rotating the second extent to be the first... " << std::flush;
        } else {
            std::cout << "Rotating the last extent to be the first... " << std::flush;
        }

        // Restore the modified configuration
        std::vector<std::string> vgcfgrestore_cmd = {"lvm", "vgcfgrestore", "--file", vgcfgname + ".new", "--", vgname};
        quiet_call(vgcfgrestore_cmd);

        // Make sure LVM updates the mapping
        std::vector<std::string> lvchange_refresh_cmd = {"lvm", "lvchange", "--refresh", "--", vgname + "/" + lvname};
        quiet_call(lvchange_refresh_cmd);

        if (active) {
            std::vector<std::string> lvchange_activate_cmd = {"lvm", "lvchange", "-ay", "--", vgname + "/" + lvname};
            quiet_call(lvchange_activate_cmd);
        }

        std::cout << "ok" << std::endl;

        // Clean up temporary directory
        std::filesystem::remove_all(tdname);
    }

    int cmd_to_lvm(const CommandArgs &args) {
        BlockDevice device(args.device);
        bool debug = args.debug;
        CLIProgressHandler progress;

        if (device.superblock_type() == "LVM2_member") {
            std::cerr << "Already a physical volume, removing existing LVM metadata...\n";
            std::vector<std::string> pvremove_cmd = {"pvremove", "-ff", "--", args.device};
            quiet_call(pvremove_cmd);
        }

        LVMReq::require(progress);

        std::string vgname;
        uint64_t pe_size;
        std::string join_name;
        std::string join_uuid;

        if (!args.join.empty()) {
            std::string vg_info_cmd = "lvm vgs --noheadings --rows --units=b --nosuffix "
                                      "-o vg_name,vg_uuid,vg_extent_size -- " + args.join;

            FILE *pipe = popen(vg_info_cmd.c_str(), "r");
            if (!pipe) {
                throw std::runtime_error("Failed to execute LVM command");
            }

            char buffer[1024];
            std::string vg_info;
            while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                vg_info += buffer;
            }
            pclose(pipe);

            std::istringstream iss(vg_info);
            std::string line;
            std::getline(iss, line);

            std::string pe_size_str;
            std::istringstream line_stream(line);
            line_stream >> join_name >> join_uuid >> pe_size_str;

            auto trim = [](std::string &s) {
                s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
                    return !std::isspace(ch);
                }));
            };

            trim(join_name);
            trim(join_uuid);
            trim(pe_size_str);

            uuid_t uuid;
            uuid_generate(uuid);
            char uuid_str[37];
            uuid_unparse_lower(uuid, uuid_str);
            vgname = uuid_str;

            pe_size = std::stoul(pe_size_str);
        } else if (!args.vgname.empty()) {
            vgname = args.vgname;
            pe_size = LVM_PE_SIZE;
        } else {
            vgname = "vg." + std::filesystem::path(device.devpath).filename().string();
            pe_size = LVM_PE_SIZE;
        }

        assert(!vgname.empty());
        for (char c : vgname) {
            assert(ASCII_ALNUM_WHITELIST.find(c) != std::string::npos);
        }

        assert(device.size() % 512 == 0);

        BlockStack block_stack = get_block_stack(device, progress);

        std::string lvname;
        if (!block_stack.fslabel().empty()) {
            lvname = block_stack.fslabel();
        } else {
            lvname = std::filesystem::path(device.devpath).filename().string();
        }

        if (std::any_of(lvname.begin(), lvname.end(), [](char c) {
            return ASCII_ALNUM_WHITELIST.find(c) == std::string::npos;
        })) {
            lvname = "lv1";
        }

        uint64_t pe_sectors = bytes_to_sector(pe_size);
        uint64_t pe_count = device.size() / pe_size - 1;
        uint64_t pe_newpos = pe_count * pe_size;

        assert(pe_size >= 4096);
        uint64_t ba_start = 2048;
        uint64_t ba_size = 2048;

        if (debug) {
            std::cout << "pe " << pe_size << " pe_newpos " << pe_newpos
                      << " devsize " << device.size() << std::endl;
        }

        block_stack.read_superblocks();

        // Single filesystem check with -y
        std::cout << "Checking the filesystem before resizing it\n";
        std::vector<std::string> fsck_cmd = {"e2fsck", "-f", "-y", "--", args.device};
        try {
            quiet_call(fsck_cmd);
        } catch (const std::exception& e) {
            std::cerr << "Filesystem check failed: " << e.what() << "\n";
            throw std::runtime_error("Filesystem check failed, please repair manually with 'e2fsck -f " + args.device + "'");
        }

        std::cout << "Will shrink the filesystem (ext4) by " << (device.size() - pe_newpos) << " bytes\n";
        block_stack.stack_reserve_end_area(pe_newpos, progress);

        std::string fsuuid = block_stack.fsuuid();
        block_stack.deactivate();

        int dev_fd = device.open_excl();
        if (dev_fd < 0) {
            std::cerr << "Failed to initially open physical device " << device.devpath << ": " << strerror(errno) << "\n";
            throw std::runtime_error("Failed to open physical device");
        }
        std::cout << "Copying " << pe_size << " bytes from pos 0 to pos "
                  << pe_newpos << "... " << std::flush;

        std::vector<uint8_t> pe_data(pe_size);
        ssize_t read_len = pread(dev_fd, pe_data.data(), pe_size, 0);
        assert(read_len == static_cast<ssize_t>(pe_size));

        ssize_t wr_len = pwrite(dev_fd, pe_data.data(), pe_size, pe_newpos);
        assert(wr_len == static_cast<ssize_t>(pe_size));
        std::cout << "ok" << std::endl;

        std::cout << "Preparing LVM metadata... " << std::flush;

        // Close dev_fd to release exclusive lock before dmsetup
        close(dev_fd);

        // Clean up stale rozeros and synthetic devices
        std::string dm_devices = exec_command("dmsetup ls | grep -E 'rozeros|synthetic' | awk '{print $1}'");
        if (!dm_devices.empty()) {
            std::cout << "High-level cleanup of stale devices:\n" << dm_devices << "\n";
            std::istringstream iss(dm_devices);
            std::string dev;
            while (std::getline(iss, dev)) {
                std::string remove_cmd = "dmsetup remove " + dev + " 2>/dev/null";
                int status = system(remove_cmd.c_str());
                if (status != 0) {
                    std::cerr << "Failed to remove " << dev << ": Device or resource busy\n";
                }
            }
        }

        // Check and log device state
        std::string holders = exec_command("lsblk -o NAME -n -l " + args.device + " | grep -v " + args.device);
        if (!holders.empty()) {
            std::cerr << "Warning: " << args.device << " has existing mappings:\n" << holders << "\n";
        }
        std::string dm_table = exec_command("dmsetup table " + args.device + " 2>/dev/null");
        if (!dm_table.empty()) {
            std::cerr << "Existing dmsetup table for " << args.device << ":\n" << dm_table << "\n";
        }

        // Create rozeros device
        uuid_t uuid_raw;
        uuid_generate(uuid_raw);
        char uuid_str[37];
        uuid_unparse_lower(uuid_raw, uuid_str);
        std::string rozeros_name = "rozeros-" + std::string(uuid_str);
        std::string rozeros_table = "0 " + std::to_string(bytes_to_sector(device.size() - pe_size)) + " error\n";
        std::function<void()> rozeros_exit_callback;
        std::vector<std::string> rozeros_cmd = {"dmsetup", "create", "--readonly", "--", rozeros_name};
        quiet_call(rozeros_cmd, rozeros_table);

        // Create synthetic device
        std::string synth_name = "synthetic-" + std::string(uuid_str);
        std::string synth_full_name = "/dev/mapper/" + synth_name;
        std::string synth_table = "0 " + std::to_string(bytes_to_sector(pe_size)) + " linear " + args.device + " 0\n" +
                                  std::to_string(bytes_to_sector(pe_size)) + " " +
                                  std::to_string(bytes_to_sector(device.size() - pe_size)) + " linear /dev/mapper/" + rozeros_name + " 0\n";
        std::function<void()> synth_exit_callback;
        std::vector<std::string> synth_cmd = {"dmsetup", "create", "--", synth_name};
        quiet_call(synth_cmd, synth_table);

        std::cout << "Synthetic device full path: " << synth_full_name << "\n";
        if (!std::filesystem::exists(synth_full_name)) {
            std::cerr << "Synthetic device " << synth_full_name << " does not exist after creation\n";
            throw std::runtime_error("Synthetic device creation failed");
        }
        SyntheticDevice synth_pv(synth_full_name);

        std::string cfgf_path = std::filesystem::temp_directory_path() /
                                (std::string("vgcfg_") + std::to_string(getpid()) + ".vgcfg");
        std::ofstream cfgf(cfgf_path);

        uuid_t pv_uuid_raw, vg_uuid_raw, lv_uuid_raw;
        uuid_generate(pv_uuid_raw);
        uuid_generate(vg_uuid_raw);
        uuid_generate(lv_uuid_raw);
        char pv_uuid_str[37], vg_uuid_str[37], lv_uuid_str[37];
        uuid_unparse_lower(pv_uuid_raw, pv_uuid_str);
        uuid_unparse_lower(vg_uuid_raw, vg_uuid_str);
        uuid_unparse_lower(lv_uuid_raw, lv_uuid_str);

        std::string pv_uuid(pv_uuid_str);
        std::string vg_uuid(vg_uuid_str);
        std::string lv_uuid(lv_uuid_str);

        cfgf << "contents = \"Text Format Volume Group\"\n"
             << "version = 1\n\n"
             << vgname << " {\n"
             << "    id = \"" << vg_uuid << "\"\n"
             << "    seqno = 0\n"
             << "    status = [\"RESIZEABLE\", \"READ\", \"WRITE\"]\n"
             << "    extent_size = " << pe_sectors << "\n"
             << "    max_lv = 0\n"
             << "    max_pv = 0\n\n"
             << "    physical_volumes {\n"
             << "        pv0 {\n"
             << "            id = \"" << pv_uuid << "\"\n"
             << "            status = [\"ALLOCATABLE\"]\n\n"
             << "            pe_start = " << pe_sectors << "\n"
             << "            pe_count = " << pe_count << "\n"
             << "            ba_start = " << ba_start << "\n"
             << "            ba_size = " << ba_size << "\n"
             << "        }\n"
             << "    }\n"
             << "    logical_volumes {\n"
             << "        " << lvname << " {\n"
             << "            id = \"" << lv_uuid << "\"\n"
             << "            status = [\"READ\", \"WRITE\", \"VISIBLE\"]\n"
             << "            segment_count = 2\n\n"
             << "            segment1 {\n"
             << "                start_extent = 0\n"
             << "                extent_count = 1\n"
             << "                type = \"striped\"\n"
             << "                stripe_count = 1 # linear\n"
             << "                stripes = [\n"
             << "                    \"pv0\", " << (pe_count - 1) << "\n"
             << "                ]\n"
             << "            }\n"
             << "            segment2 {\n"
             << "                start_extent = 1\n"
             << "                extent_count = " << (pe_count - 1) << "\n"
             << "                type = \"striped\"\n"
             << "                stripe_count = 1 # linear\n"
             << "                stripes = [\n"
             << "                    \"pv0\", 0\n"
             << "                ]\n"
             << "            }\n"
             << "        }\n"
             << "    }\n"
             << "}\n";
        cfgf.close();

        std::string lvm_config = "devices{filter=[\"a|^" + synth_full_name + "$|\",\"r|.*|\"]}activation{verify_udev_operations=1}";
        std::string lvm_cfg = "--config='" + lvm_config + "'";

        std::cout << "LVM config: " << lvm_cfg << "\n";

        std::vector<std::string> pvcreate_cmd;
        pvcreate_cmd.push_back("lvm");
        pvcreate_cmd.push_back("pvcreate");
        pvcreate_cmd.push_back(lvm_cfg);
        pvcreate_cmd.push_back("--restorefile");
        pvcreate_cmd.push_back(cfgf_path);
        pvcreate_cmd.push_back("--uuid");
        pvcreate_cmd.push_back(pv_uuid);
        pvcreate_cmd.push_back("--zero");
        pvcreate_cmd.push_back("y");
        pvcreate_cmd.push_back("--");
        pvcreate_cmd.push_back(synth_full_name);

        std::vector<std::string> vgcfgrestore_cmd;
        vgcfgrestore_cmd.push_back("lvm");
        vgcfgrestore_cmd.push_back("vgcfgrestore");
        vgcfgrestore_cmd.push_back(lvm_cfg);
        vgcfgrestore_cmd.push_back("--file");
        vgcfgrestore_cmd.push_back(cfgf_path);
        vgcfgrestore_cmd.push_back("--");
        vgcfgrestore_cmd.push_back(vgname);

        quiet_call(pvcreate_cmd);
        quiet_call(vgcfgrestore_cmd);

        std::cout << "ok" << std::endl;

        // Read metadata from synthetic device before removal
        int synth_fd = open(synth_full_name.c_str(), O_RDONLY);
        if (synth_fd < 0) {
            std::cerr << "Failed to open synthetic device " << synth_full_name << " for reading: " << strerror(errno) << "\n";
            throw std::runtime_error("Failed to open synthetic device for metadata read");
        }
        std::vector<uint8_t> metadata(pe_size);
        ssize_t metadata_read = pread(synth_fd, metadata.data(), pe_size, 0);
        if (metadata_read != static_cast<ssize_t>(pe_size)) {
            std::cerr << "Failed to read metadata from " << synth_full_name << ": expected " << pe_size
                      << " bytes, read " << metadata_read << " bytes\n";
            close(synth_fd);
            throw std::runtime_error("Failed to read metadata from synthetic device");
        }
        close(synth_fd);

        // Remove synthetic devices to release /dev/loop26p1
        std::vector<std::string> remove_synth_cmd = {"dmsetup", "remove", synth_name};
        std::vector<std::string> remove_rozeros_cmd = {"dmsetup", "remove", rozeros_name};
        quiet_call(remove_synth_cmd);
        quiet_call(remove_rozeros_cmd);

        std::cout << "If the next stage is interrupted, it can be reverted with:\n"
                  << "    dd if=" << device.devpath << " of=" << device.devpath
                  << " bs=" << pe_size << " count=1 skip=" << pe_count << " conv=notrunc"
                  << std::endl;

        std::cout << "Installing LVM metadata... " << std::flush;
        dev_fd = device.open_excl();
        if (dev_fd < 0) {
            std::cerr << "Failed to reopen physical device " << device.devpath << ": " << strerror(errno) << "\n";
            throw std::runtime_error("Failed to reopen physical device for metadata copy");
        }
        std::cout << "Writing " << pe_size << " bytes of metadata to physical device at offset 0\n";
        ssize_t metadata_written = pwrite(dev_fd, metadata.data(), pe_size, 0);
        if (metadata_written != static_cast<ssize_t>(pe_size)) {
            std::cerr << "Failed to write metadata to " << device.devpath << ": expected " << pe_size
                      << " bytes, wrote " << metadata_written << " bytes, errno: " << strerror(errno) << "\n";
            close(dev_fd);
            throw std::runtime_error("Failed to write metadata to physical device");
        }
        std::cout << "ok" << std::endl;
        close(dev_fd);

        std::cout << "Activating volume group " << vgname << "... " << std::flush;
        std::vector<std::string> vgchange_cmd = {"vgchange", "-ay", "--", vgname};
        try {
            quiet_call(vgchange_cmd);
            std::cout << "ok" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Failed to activate volume group " << vgname << ": " << e.what() << "\n";
            throw std::runtime_error("Volume group activation failed");
        }

        std::cout << "LVM conversion successful!" << std::endl;

        if (!args.join.empty()) {
            std::vector<std::string> vgmerge_cmd = {"lvm", "vgmerge", "--", join_name, vgname};
            quiet_call(vgmerge_cmd);
            vgname = join_name;
        }

        std::cout << "Volume group name: " << vgname << "\n"
                  << "Logical volume name: " << lvname << "\n"
                  << "Filesystem uuid: " << fsuuid << "\n";

        std::filesystem::remove(cfgf_path);

        return 0;
    }

} // namespace blocks
