#include "maintboot_operations.h"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <unistd.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <memory> // For std::unique_ptr

namespace blocks {

// Factory function to create the appropriate Filesystem instance
    std::unique_ptr<Filesystem> create_filesystem(BlockDevice& device) {
        std::string fs_type = device.superblock_type(); // Assumes this method exists

        if (fs_type == "ext2" || fs_type == "ext3" || fs_type == "ext4") {
            return std::make_unique<ExtFS>(device);
        } else if (fs_type == "xfs") {
            return std::make_unique<XFS>(device);
        } else if (fs_type == "btrfs") {
            return std::make_unique<BtrFS>(device);
        } else if (fs_type == "reiserfs") {
            return std::make_unique<ReiserFS>(device);
        } else if (fs_type == "nilfs2") {
            return std::make_unique<NilFS>(device);
        } else if (fs_type == "swap") {
            return std::make_unique<Swap>(device);
        } else {
            throw std::runtime_error("Unsupported filesystem type: " + fs_type);
        }
    }

    int call_maintboot(BlockDevice device, const std::string& command,
                       const std::map<std::string, std::string>& args) {
        // Use the factory function instead of direct instantiation
        std::unique_ptr<Filesystem> fs = create_filesystem(device);
        std::string fsuuid = fs->fsuuid();

        if (fsuuid.empty()) {
            std::cerr << "Device " << device.devpath << " doesn't have a UUID" << std::endl;
            return 1;
        }

        // Create a JSON object with the command and arguments
        nlohmann::json json_args;
        json_args["command"] = command;
        json_args["device"] = fsuuid;

        // Add any additional arguments
        for (const auto& [key, value] : args) {
            json_args[key] = value;
        }

        // URL encode the JSON string
        std::string json_str = json_args.dump();

        CURL* curl = curl_easy_init();
        if (!curl) {
            std::cerr << "Failed to initialize curl" << std::endl;
            return 1;
        }

        char* encoded = curl_easy_escape(curl, json_str.c_str(), json_str.length());
        std::string encoded_args(encoded);
        curl_free(encoded);
        curl_easy_cleanup(curl);

        // Build the maintboot command
        std::vector<std::string> cmd = {
                "maintboot",
                "--pkgs",
                "python3-blocks util-linux dash mount base-files libc-bin nilfs-tools reiserfsprogs xfsprogs e2fsprogs btrfs-tools lvm2 cryptsetup-bin bcache-tools",
                "--initscript",
                "/usr/share/blocks/maintboot.init",
                "--append",
                "BLOCKS_ARGS=" + encoded_args
        };

        try {
            quiet_call(cmd);
            return 0;
        } catch (const std::exception& e) {
            std::cerr << "Failed to execute maintboot: " << e.what() << std::endl;
            return 1;
        }
    }

    nlohmann::json parse_maintboot_args() {
        const char* env_args = std::getenv("BLOCKS_ARGS");
        if (!env_args) {
            throw std::runtime_error("BLOCKS_ARGS environment variable not set");
        }

        // URL decode the arguments
        CURL* curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Failed to initialize curl");
        }

        int decoded_length = 0;
        char* decoded = curl_easy_unescape(curl, env_args, 0, &decoded_length);
        std::string decoded_args(decoded, decoded_length);
        curl_free(decoded);
        curl_easy_cleanup(curl);

        // Parse the JSON
        try {
            return nlohmann::json::parse(decoded_args);
        } catch (const nlohmann::json::exception& e) {
            throw std::runtime_error("Failed to parse BLOCKS_ARGS as JSON: " + std::string(e.what()));
        }
    }

    void prepare_maintboot_environment() {
        // Wait for devices to come up (30s max)
        std::vector<std::string> settle_cmd = {"udevadm", "settle", "--timeout=30"};
        quiet_call(settle_cmd);

        // Activate LVM volumes
        std::vector<std::string> lvm_cmd = {"lvm", "vgchange", "-ay"};
        quiet_call(lvm_cmd);
    }

    int cmd_maintboot_impl(int argc, char* argv[]) {
        try {
            // Parse the BLOCKS_ARGS environment variable
            nlohmann::json args = parse_maintboot_args();

            // Verify that the command is what we expect
            if (args["command"] != "to-bcache") {
                throw std::runtime_error("Unsupported command: " + args["command"].get<std::string>());
            }

            // Prepare the environment
            prepare_maintboot_environment();

            // Get the device by UUID
            std::string device_uuid = args["device"].get<std::string>();
            BlockDevice device = BlockDevice::by_uuid(device_uuid);

            // Create a map of arguments for the to-bcache command
            std::map<std::string, std::string> bcache_args;

            // Copy any additional arguments
            for (auto it = args.begin(); it != args.end(); ++it) {
                if (it.key() != "command" && it.key() != "device" && it.key() != "maintboot") {
                    bcache_args[it.key()] = it.value().get<std::string>();
                }
            }

            // Set maintboot to false to avoid recursion
            bcache_args["maintboot"] = "false";

            // Call the to-bcache command
            std::vector<std::string> cmd = {"blocks", "to-bcache", device.devpath};

            // Add any additional arguments
            for (const auto& [key, value] : bcache_args) {
                if (key == "join" && !value.empty()) {
                    cmd.push_back("--join");
                    cmd.push_back(value);
                } else if (key == "debug" && value == "true") {
                    cmd.push_back("--debug");
                }
            }

            quiet_call(cmd);
            return 0;
        } catch (const std::exception& e) {
            std::cerr << "Error in maintboot implementation: " << e.what() << std::endl;
            return 1;
        }
    }

} // namespace blocks