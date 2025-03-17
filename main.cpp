#include <iostream>
#include <string>
#include <vector>
#include <regex>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <getopt.h>
#include <cassert>
#include <memory>

#include "blocks_types.h"
#include "block_device.h"
#include "block_stack.h"
#include "lvm_operations.h"
#include "bcache_operations.h"
#include "resize_operations.h"
#include "maintboot_operations.h"

namespace blocks {
    void print_help() {
        std::cout << "Usage: blocks [options] command [command_options]" << std::endl;
        std::cout << std::endl;
        std::cout << "Commands:" << std::endl;
        std::cout << "  to-lvm, lvmify    Convert to LVM" << std::endl;
        std::cout << "  to-bcache         Convert to bcache" << std::endl;
        std::cout << "  resize            Resize a device or filesystem" << std::endl;
        std::cout << "  rotate            Rotate LV contents to start at the second PE" << std::endl;
        std::cout << "  maintboot-impl    Internal command for maintenance boot" << std::endl;
        std::cout << std::endl;
        std::cout << "Global options:" << std::endl;
        std::cout << "  --debug           Enable debug output" << std::endl;
        std::cout << std::endl;
        std::cout << "Command options:" << std::endl;
        std::cout << "  to-lvm, lvmify:" << std::endl;
        std::cout << "    --vg-name NAME  Use specified volume group name" << std::endl;
        std::cout << "    --join VG       Join existing volume group" << std::endl;
        std::cout << std::endl;
        std::cout << "  to-bcache:" << std::endl;
        std::cout << "    --join UUID     Join existing cache set" << std::endl;
        std::cout << "    --maintboot     Use maintenance boot for conversion" << std::endl;
        std::cout << std::endl;
        std::cout << "  resize:" << std::endl;
        std::cout << "    --resize-device Resize the device, not just the contents" << std::endl;
        std::cout << "    SIZE            New size in byte units (bkmgtpe suffixes accepted)" << std::endl;
    }

    uint64_t parse_size_arg(const std::string& size) {
        static const std::regex SIZE_RE("^(\\d+)([bkmgtpe])?\\Z");
        std::smatch match;

        // Check if the size string matches the expected pattern
        if (!std::regex_match(size, match, SIZE_RE)) {
            throw std::invalid_argument(
                    "Size must be a decimal integer and a one-character unit suffix (bkmgtpe)");
        }

        // Convert the matched number (match[1]) to uint64_t
        uint64_t val = std::stoull(match[1].str());

        // Get the unit as a string; use "b" if no unit is matched
        std::string unit = match[2].matched ? match[2].str() : "b";

        // Define units as a std::string to use the find method
        std::string units = "bkmgtpe";
        size_t pos = units.find(unit[0]);

        // Check if the unit is valid
        if (pos == std::string::npos) {
            throw std::invalid_argument("Invalid unit");
        }

        // Calculate the size by multiplying by 1024^pos
        return val * static_cast<uint64_t>(std::pow(1024, pos));
    }
    int cmd_rotate(const CommandArgs& args) {
        BlockDevice device(args.device);
        bool debug = args.debug;
        CLIProgressHandler progress;

        std::string pe_size_str;
        std::vector<std::string> cmd = {
                "lvm", "lvs", "--noheadings", "--rows", "--units=b", "--nosuffix",
                "-o", "vg_extent_size", "--", device.devpath
        };

        FILE* pipe = popen(cmd[0].c_str(), "r");
        if (!pipe) {
            std::cerr << "Failed to execute command" << std::endl;
            return 1;
        }

        char buffer[128];
        if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            pe_size_str = buffer;
            pe_size_str.erase(0, pe_size_str.find_first_not_of(" \t\n\r\f\v"));
            pe_size_str.erase(pe_size_str.find_last_not_of(" \t\n\r\f\v") + 1);
        }
        pclose(pipe);

        uint64_t pe_size = std::stoull(pe_size_str);

        if (device.superblock_at(pe_size).empty()) {
            std::cerr << "No superblock on the second PE, exiting" << std::endl;
            return 1;
        }

        rotate_lv(device, device.size(), debug, true);
        return 0;
    }

    int main(int argc, char* argv[]) {
        try {
            assert(true);
        } catch (const std::exception&) {
            std::cerr << "Assertions need to be enabled" << std::endl;
            return 2;
        }

        if (argc < 2) {
            print_help();
            return 0;
        }

        CommandArgs args;
        int option_index = 0;
        int c;

        static struct option long_options[] = {
                {"debug", no_argument, 0, 'd'},
                {"vg-name", required_argument, 0, 'v'},
                {"join", required_argument, 0, 'j'},
                {"maintboot", no_argument, 0, 'm'},
                {"resize-device", no_argument, 0, 'r'},
                {"help", no_argument, 0, 'h'},
                {0, 0, 0, 0}
        };

        while ((c = getopt_long(argc, argv, "dv:j:mrh", long_options, &option_index)) != -1) {
            switch (c) {
                case 'd':
                    args.debug = true;
                    break;
                case 'v':
                    args.vgname = optarg;
                    break;
                case 'j':
                    args.join = optarg;
                    break;
                case 'm':
                    args.maintboot = true;
                    break;
                case 'r':
                    args.resize_device = true;
                    break;
                case 'h':
                    print_help();
                    return 0;
                case '?':
                    return 1;
                default:
                    abort();
            }
        }

        if (optind >= argc) {
            std::cerr << "Missing command" << std::endl;
            print_help();
            return 1;
        }

        args.command = argv[optind++];

        if (args.command == "to-lvm" || args.command == "lvmify") {
            if (optind >= argc) {
                std::cerr << "Missing device argument" << std::endl;
                return 1;
            }
            args.device = argv[optind++];
            return cmd_to_lvm(args);
        }
        else if (args.command == "to-bcache") {
            if (optind >= argc) {
                std::cerr << "Missing device argument" << std::endl;
                return 1;
            }
            args.device = argv[optind++];

            BlockDevice device(args.device);
            CLIProgressHandler progress;

            if (device.has_bcache_superblock()) {
                std::cerr << "Device " << device.devpath << " already has a bcache super block." << std::endl;
                return 1;
            }

            BCacheReq::require(progress);

            if (args.maintboot) {
                return call_maintboot(device, "to-bcache", {
                        {"debug", args.debug ? "true" : "false"},
                        {"join", args.join}
                });
            }
            else if (device.is_partition()) {
                return part_to_bcache(device, args.debug, progress, args.join);
            }
            else if (device.is_lv()) {
                return lv_to_bcache(device, args.debug, progress, args.join);
            }
            else if (device.superblock_type() == "crypto_LUKS") {
                return luks_to_bcache(device, args.debug, progress, args.join);
            }
            else {
                std::cerr << "Device " << device.devpath
                          << " is not a partition, a logical volume, or a LUKS volume" << std::endl;
                return 1;
            }
        }
        else if (args.command == "resize") {
            if (optind >= argc) {
                std::cerr << "Missing device argument" << std::endl;
                return 1;
            }
            args.device = argv[optind++];

            if (optind >= argc) {
                std::cerr << "Missing size argument" << std::endl;
                return 1;
            }

            try {
                args.newsize = parse_size_arg(argv[optind++]);
            } catch (const std::invalid_argument& e) {
                std::cerr << e.what() << std::endl;
                return 1;
            }

            ResizeArgs resize_args = {
                    .device = args.device,
                    .newsize = args.newsize,
                    .resize_device = args.resize_device,
                    .debug = args.debug
            };

            return cmd_resize(resize_args);
        }
        else if (args.command == "rotate") {
            if (optind >= argc) {
                std::cerr << "Missing device argument" << std::endl;
                return 1;
            }
            args.device = argv[optind++];
            return cmd_rotate(args);
        }
        else if (args.command == "maintboot-impl") {
            return cmd_maintboot_impl(argc, argv);
        }
        else {
            std::cerr << "Unknown command: " << args.command << std::endl;
            print_help();
            return 1;
        }

        return 0;
    }

    int script_main() {
        return main(0, nullptr);
    }

} // namespace blocks

int main(int argc, char* argv[]) {
    return blocks::main(argc, argv);
}