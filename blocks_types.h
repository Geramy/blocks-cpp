#ifndef BLOCKS_TYPES_H
#define BLOCKS_TYPES_H

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <uuid/uuid.h>
#include <vector>
#include <sys/wait.h>
#include <pcrecpp.h>

namespace blocks {

// 4MiB PE, for vgmerge compatibility
constexpr uint64_t LVM_PE_SIZE = 4ULL * 1024ULL * 1024ULL;

const std::string ASCII_ALNUM_WHITELIST = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.";

const std::array<uint8_t, 16> BCACHE_MAGIC = {
    0xc6, 0x85, 0x73, 0xf6, 0x4e, 0x1a, 0x45, 0xca,
    0x82, 0x65, 0xf5, 0x7f, 0x48, 0xba, 0x6d, 0x81
};

// Fairly strict, snooping an incorrect mapping would be bad
// dm_crypt_re with PCRE
    const pcrecpp::RE dm_crypt_re(
            "^0 (?P<plainsize>\\d+) crypt (?P<cipher>[a-z0-9:-]+) 0+ 0 "
            "(?P<major>\\d+):(?P<minor>\\d+) (?P<offset>\\d+)(?P<options> [^\\n]*)?\\n\\Z");

// dm_kpartx_re with PCRE
    const pcrecpp::RE dm_kpartx_re(
            "^0 (?P<partsize>\\d+) linear "
            "(?P<major>\\d+):(?P<minor>\\d+) (?P<offset>\\d+)\\n\\Z");

inline uint64_t bytes_to_sector(uint64_t by) {
    uint64_t sectors = by / 512;
    assert(by % 512 == 0);
    return sectors;
}

inline uint64_t intdiv_up(uint64_t num, uint64_t denom) {
    return (num - 1) / denom + 1;
}

inline uint64_t align_up(uint64_t size, uint64_t align) {
    return intdiv_up(size, align) * align;
}

inline uint64_t align(uint64_t size, uint64_t align) {
    return (size / align) * align;
}

class UnsupportedSuperblock : public std::exception {
public:
    std::string device;
    std::map<std::string, std::string> kwargs;

    UnsupportedSuperblock(const std::string& device, const std::map<std::string, std::string>& kwargs = {})
        : device(device), kwargs(kwargs) {}

    const char* what() const noexcept override {
        static std::string message;
        message = "UnsupportedSuperblock: device=" + device;
        for (const auto& [key, value] : kwargs) {
            message += ", " + key + "=" + value;
        }
        return message.c_str();
    }
};

class UnsupportedLayout : public std::exception {
public:
    const char* what() const noexcept override {
        return "UnsupportedLayout";
    }
};

class CantShrink : public std::exception {
public:
    const char* what() const noexcept override {
        return "CantShrink";
    }
};

class OverlappingPartition : public std::exception {
public:
    const char* what() const noexcept override {
        return "OverlappingPartition";
    }
};

// SQLa, compatible license
    template <typename T, typename Class>
    class memoized_property {
    private:
        using getter_type = T (Class::*)();  // Member function pointer
        getter_type fget;
        std::string name;
        std::string doc;

    public:
        memoized_property(getter_type fget, const std::string& name, const std::string& doc = "")
                : fget(fget), name(name), doc(doc) {}

        T& get(Class* obj, std::unordered_map<std::string, T>& cache) {
            auto it = cache.find(name);
            if (it == cache.end()) {
                T result = (obj->*fget)();  // Call member function pointer
                auto [inserted_it, _] = cache.emplace(name, std::move(result));
                return inserted_it->second;
            }
            return it->second;
        }

        void reset(Class* obj, std::unordered_map<std::string, T>& cache) {
            cache.erase(name);
        }
    };

    inline std::string exec_command(const std::string& cmd) {
        std::array<char, 128> buffer;
        std::string result;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
        if (!pipe) throw std::runtime_error("popen failed: " + cmd);
        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
            result += buffer.data();
        }
        return result;
    }
    inline std::string join_cmd(const std::vector<std::string>& cmd) {
        std::string result;
        for (size_t i = 0; i < cmd.size(); ++i) {
            result += cmd[i];
            if (i < cmd.size() - 1) result += " ";
        }
        return result;
    }
    inline void quiet_call(const std::vector<std::string>& cmd, const std::string& table = "") {
        std::string full_cmd = join_cmd(cmd);
        std::cout << "Executing: " << full_cmd << "\n"; // Debug
        if (!table.empty()) std::cout << "Table:\n" << table << "\n"; // Debug

        FILE* pipe = popen(full_cmd.c_str(), "w");
        if (!pipe) {
            std::cerr << "popen failed: " << full_cmd << "\n";
            throw std::runtime_error("popen failed: " + full_cmd);
        }

        if (!table.empty()) {
            if (fputs(table.c_str(), pipe) == EOF) {
                pclose(pipe);
                std::cerr << "Failed to write table to " << full_cmd << "\n";
                throw std::runtime_error("Failed to write table to " + full_cmd);
            }
        }

        int status = pclose(pipe);
        if (status != 0) {
            std::string stderr_output = exec_command("dmesg | tail -n 5"); // Rough stderr approximation
            std::cerr << "Command failed with status " << status << "\nStderr:\n" << stderr_output << "\n";
            throw std::runtime_error("Command failed: " + full_cmd);
        }
    }
/*
inline void quiet_call(const std::vector<std::string>& cmd, const std::string& input = "") {
    std::vector<const char*> c_cmd;
    for (const auto& arg : cmd) {
        c_cmd.push_back(arg.c_str());
    }
    c_cmd.push_back(nullptr);

    int pipe_stdin[2] = {-1, -1};
    int pipe_stdout[2] = {-1, -1};
    int pipe_stderr[2] = {-1, -1};

    if (!input.empty() && pipe(pipe_stdin) == -1) {
        throw std::runtime_error("Failed to create stdin pipe");
    }
    if (pipe(pipe_stdout) == -1) {
        throw std::runtime_error("Failed to create stdout pipe");
    }
    if (pipe(pipe_stderr) == -1) {
        throw std::runtime_error("Failed to create stderr pipe");
    }

    pid_t pid = fork();
    if (pid == -1) {
        throw std::runtime_error("Failed to fork process");
    }

    if (pid == 0) { // Child process
        if (!input.empty()) {
            close(pipe_stdin[1]);
            dup2(pipe_stdin[0], STDIN_FILENO);
            close(pipe_stdin[0]);
        } else {
            int dev_null = open("/dev/null", O_RDONLY);
            dup2(dev_null, STDIN_FILENO);
            close(dev_null);
        }

        close(pipe_stdout[0]);
        dup2(pipe_stdout[1], STDOUT_FILENO);
        close(pipe_stdout[1]);

        close(pipe_stderr[0]);
        dup2(pipe_stderr[1], STDERR_FILENO);
        close(pipe_stderr[1]);

        execvp(c_cmd[0], const_cast<char* const*>(c_cmd.data()));
        exit(1); // If execvp fails
    }

    // Parent process
    if (!input.empty()) {
        close(pipe_stdin[0]);
        write(pipe_stdin[1], input.c_str(), input.size());
        close(pipe_stdin[1]);
    }

    close(pipe_stdout[1]);
    close(pipe_stderr[1]);

    std::string output, error;
    char buffer[4096];
    ssize_t bytes_read;

    while ((bytes_read = read(pipe_stdout[0], buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        output += buffer;
    }
    close(pipe_stdout[0]);

    while ((bytes_read = read(pipe_stderr[0], buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        error += buffer;
    }
    close(pipe_stderr[0]);

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        std::cerr << "Command " << cmd[0] << " has failed with status " << WEXITSTATUS(status) << std::endl
                  << "Standard output:" << std::endl << output << std::endl
                  << "Standard error:" << std::endl << error << std::endl;
        throw std::runtime_error("Command failed with status " + std::to_string(WEXITSTATUS(status)));
    }
}
*/
class MissingRequirement : public std::exception {
public:
    const char* what() const noexcept override {
        return "MissingRequirement";
    }
};

    inline void mk_dm(const std::string& devname, const std::string& table, bool readonly, std::function<void()>& exit_callback) {
        bool needs_udev_fallback = false;
        std::vector<std::string> cmd = {"dmsetup", "create", "--noudevsync"};

        if (readonly) {
            cmd.insert(cmd.begin() + 2, "--readonly"); // Insert before "--noudevsync"
        }
        cmd.push_back("--");
        cmd.push_back(devname);

        try {
            quiet_call(cmd, table);
        } catch (const std::exception& e) {
            std::cerr << "Initial dmsetup failed: " << e.what() << "\nTrying fallback...\n";
            needs_udev_fallback = true;
            cmd = {"dmsetup", "create", "--verifyudev"}; // Reset cmd
            if (readonly) {
                cmd.insert(cmd.begin() + 2, "--readonly");
            }
            cmd.push_back("--");
            cmd.push_back(devname);
            quiet_call(cmd, table); // This must succeed or throw
        }

        std::vector<std::string> remove_cmd = {"dmsetup", "remove", "--noudevsync", "--", devname};
        if (needs_udev_fallback) {
            remove_cmd[2] = "--verifyudev"; // Replace, not insert
        }

        exit_callback = [remove_cmd]() {
            try {
                quiet_call(remove_cmd);
            } catch (const std::exception& e) {
                std::cerr << "Warning: Failed to remove device: " << e.what() << "\n";
            }
        };
    }

inline std::string aftersep(const std::string& line, const std::string& sep) {
    size_t pos = line.find(sep);
    if (pos == std::string::npos) {
        return "";
    }
    std::string result = line.substr(pos + sep.length());
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    return result;
}

inline std::string devpath_from_sysdir(const std::string& sd) {
    std::ifstream uevent(sd + "/uevent");
    std::string line;
    while (std::getline(uevent, line)) {
        if (line.find("DEVNAME=") == 0) {
            return "/dev/" + aftersep(line, "=");
        }
    }
    return "";
}

class ProgressListener {
public:
    virtual void notify(const std::string& msg) = 0;
    virtual void bail(const std::string& msg, const std::exception& err) = 0;
    virtual ~ProgressListener() = default;
};

class DefaultProgressHandler : public ProgressListener {
public:
    void notify(const std::string& msg) override {
        std::cout << "[INFO] " << msg << std::endl;
    }

    void bail(const std::string& msg, const std::exception& err) override {
        std::cerr << "[ERROR] " << msg << std::endl;
        throw err;
    }
};

class CLIProgressHandler : public ProgressListener {
public:
    void notify(const std::string& msg) override {
        std::cout << msg << std::endl;
    }

    void bail(const std::string& msg, const std::exception& err) override {
        std::cerr << msg << std::endl;
        exit(2);
    }
};

inline bool starts_with_word(const std::string& line, const std::string& word) {
    if (line.find(word) != 0) {
        return false;
    }

    size_t word_len = word.length();
    return line.length() == word_len || std::isspace(line[word_len]);
}
    class Requirement {
    public:
        static void require(const char* cmd, const char* pkg, ProgressListener& progress) {
            // Check if cmd contains a slash
            if (std::string(cmd).find('/') != std::string::npos) {
                progress.bail("Command '" + std::string(cmd) + "' should not contain a slash", MissingRequirement());
                return; // Optional: depending on whether bail exits or not
            }

            // Check if the command exists using 'which' (Unix-like systems)
            std::string check_cmd = "which " + std::string(cmd) + " > /dev/null 2>&1";
            int result = std::system(check_cmd.c_str());
            if (result != 0) {
                std::string message = "Command '" + std::string(cmd) + "' not found, please install the " + std::string(pkg) + " package";
                progress.bail(message, MissingRequirement());
            }
        }
    };

    class LVMReq : public Requirement {
    public:
        static constexpr const char* cmd = "lvm";
        static constexpr const char* pkg = "lvm2";

        static void require(ProgressListener& progress) {
            Requirement::require(cmd, pkg, progress);
        }
    };

    class BCacheReq : public Requirement {
    public:
        static constexpr const char* cmd = "make-bcache";
        static constexpr const char* pkg = "bcache-tools";

        static void require(ProgressListener& progress) {
            Requirement::require(cmd, pkg, progress);
        }
    };

} // namespace blocks

#endif // BLOCKS_TYPES_H
