#ifndef MAINTBOOT_OPERATIONS_H
#define MAINTBOOT_OPERATIONS_H

#include "blocks_types.h"
#include "block_device.h"
#include "filesystem.h"
#include <string>
#include <map>
#include <nlohmann/json.hpp>

namespace blocks {

/**
 * Call the maintenance boot system with the specified command and arguments
 * 
 * @param device The block device to operate on
 * @param command The command to execute in maintenance boot
 * @param args Additional arguments for the command
 * @return int Return code (0 for success, non-zero for failure)
 */
int call_maintboot(BlockDevice device, const std::string& command, 
                  const std::map<std::string, std::string>& args = {});

/**
 * Implementation of the maintenance boot command
 * 
 * This function is called when the system boots into maintenance mode
 * to perform the requested operation.
 * 
 * @param args Command line arguments
 * @return int Return code (0 for success, non-zero for failure)
 */
int cmd_maintboot_impl(int argc, char* argv[]);

/**
 * Parse the BLOCKS_ARGS environment variable to extract command arguments
 * 
 * @return nlohmann::json Parsed arguments
 */
nlohmann::json parse_maintboot_args();

/**
 * Wait for devices to come up and activate LVM volumes
 */
void prepare_maintboot_environment();

} // namespace blocks

#endif // MAINTBOOT_OPERATIONS_H
