#include <argp.h>
#include <regex>
#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <sstream>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <cassert>
#include <uuid/uuid.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

// 4MiB PE, for vgmerge compatibility
const size_t LVM_PE_SIZE = 4 * std::pow(1024, 2);

const std::string ASCII_ALNUM_WHITELIST =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.";

const unsigned char BCACHE_MAGIC[] = {0xc6, 0x85, 0x73, 0xf6, 0x4e, 0x1a, 0x45, 0xca, 0x82, 0x65, 0xf5, 0x7f, 0x48,
                                      0xba, 0x6d, 0x81};

// Fairly strict, snooping an incorrect mapping would be bad
static const std::regex dm_crypt_re(
        R"(^0 (?P<plainsize>\d+) crypt (?P<cipher>[a-z0-9:-]+) 0+ 0 (?P<major>\d+):(?P<minor>\d+) (?P<offset>\d+)(?P<options> [^\n]*)?\n\Z)",
        std::regex_constants::ECMAScript);

static const std::regex dm_kpartx_re(R"(^0 (?P<partsize>\d+) linear (?P<major>\d+):(?P<minor>\d+) (?P<offset>\d+)\n\Z)",
                                     std::regex_constants::ECMAScript);

size_t bytes_to_sector(size_t by) {
    size_t rem = by % 512;
    assert(rem == 0);
    return by / 512;
}

size_t intdiv_up(size_t num, size_t denom) {
    return (num - 1) / denom + 1;
}

size_t align_up(size_t size, size_t align) {
    return intdiv_up(size, align) * align;
}

size_t align(size_t size, size_t align) {
    return (size / align) * align;
}

class UnsupportedSuperblockException : public std::runtime_error {
public:
    UnsupportedSuperblockException(const std::string &device)
            : std::runtime_error("Unsupported superblock on device: " + device), device(device) {}

    const std::string device;
};

class UnsupportedLayoutException : public std::runtime_error {
public:
    UnsupportedLayoutException() : std::runtime_error("Unsupported layout") {}
};

class CantShrinkException : public std::runtime_error {
public:
    CantShrinkException() : std::runtime_error("Cannot shrink") {}
};

class OverlappingPartitionException : public std::runtime_error {
public:
    OverlappingPartitionException() : std::runtime_error("Overlapping partition detected") {}
};

class MissingRequirementException : public std::runtime_error {
public:
    MissingRequirementException() : std::runtime_error("Missing requirement") {}
};

class Requirement {
public:
    static void require(std::ostream &progress, const std::string &cmd, const std::string &pkg) {
        if (system((std::string("command -v ") + cmd + " >/dev/null 2>&1").c_str()) != 0) {
            progress << "Command \"" << cmd << "\" not found. Please install the \"" << pkg << "\" package.\n";
            throw MissingRequirementException();
        }
    }
};

std::string aftersep(const std::string &line, char sep) {
    auto pos = line.find(sep);
    if (pos != std::string::npos) {
        return line.substr(pos + 1);
    }
    return {};
}

// The converted co\
// ]de continues the same, due to its sheer line size;
// you may split this into several snippets if possible to handle appropriately.