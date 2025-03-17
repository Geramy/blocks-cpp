#include "synthetic_device.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fstream>
#include <iostream>
#include <cstring>
#include <filesystem>
#include <random>
#include <sstream>

namespace blocks {

    SyntheticDevice::SyntheticDevice(const std::string &devpath)
            : BlockDevice(devpath), writable_hdr_size(0), rz_size(0), writable_end_size(0) {
    }

    void SyntheticDevice::copy_to_physical(
            int dev_fd,
            uint64_t shift_by,
            uint64_t reserved_area,
            bool other_device) {

        assert(data.size() == writable_hdr_size + writable_end_size);

        std::vector<uint8_t> start_data(data.begin(), data.begin() + writable_hdr_size);
        std::vector<uint8_t> end_data(data.begin() + writable_hdr_size, data.end());

        uint64_t wrend_offset = writable_hdr_size + rz_size + shift_by;
        uint64_t size = writable_hdr_size + rz_size + writable_end_size;

        if (shift_by < 0) {
            assert(!other_device);
            shift_by += size;
        }

        // Only enforce shift_by >= reserved_area if reserved_area > 0
        if (reserved_area != 0) {
            assert(shift_by >= reserved_area);
            assert(wrend_offset >= reserved_area);
        }

        if (!other_device) {
            assert(0 <= shift_by && shift_by + writable_hdr_size <= size);
            if (writable_end_size != 0) {
                assert(0 <= wrend_offset && wrend_offset + writable_end_size <= size);
            }
        }

        std::cout << "Writing " << writable_hdr_size << " bytes to physical device at offset " << shift_by << "\n";
        ssize_t written = pwrite(dev_fd, start_data.data(), writable_hdr_size, shift_by);
        if (written != static_cast<ssize_t>(writable_hdr_size)) {
            std::cerr << "Failed to write to physical device: expected " << writable_hdr_size
                      << " bytes, wrote " << written << " bytes, errno: " << strerror(errno) << "\n";
            throw std::runtime_error("Write to physical device failed");
        }

        std::vector<uint8_t> read_back(writable_hdr_size);
        ssize_t read_bytes = pread(dev_fd, read_back.data(), writable_hdr_size, shift_by);
        if (read_bytes != static_cast<ssize_t>(writable_hdr_size)) {
            std::cerr << "Failed to read back from physical device: expected " << writable_hdr_size
                      << " bytes, read " << read_bytes << " bytes\n";
            throw std::runtime_error("Read back from physical device failed");
        }
        assert(memcmp(start_data.data(), read_back.data(), writable_hdr_size) == 0);

        if (writable_end_size != 0) {
            std::cout << "Writing " << writable_end_size << " bytes to physical device at offset " << wrend_offset << "\n";
            written = pwrite(dev_fd, end_data.data(), writable_end_size, wrend_offset);
            if (written != static_cast<ssize_t>(writable_end_size)) {
                std::cerr << "Failed to write end data to physical device: expected " << writable_end_size
                          << " bytes, wrote " << written << " bytes, errno: " << strerror(errno) << "\n";
                throw std::runtime_error("Write end data to physical device failed");
            }

            read_back.resize(writable_end_size);
            read_bytes = pread(dev_fd, read_back.data(), writable_end_size, wrend_offset);
            if (read_bytes != static_cast<ssize_t>(writable_end_size)) {
                std::cerr << "Failed to read back end data: expected " << writable_end_size
                          << " bytes, read " << read_bytes << " bytes\n";
                throw std::runtime_error("Read back end data failed");
            }
            assert(memcmp(end_data.data(), read_back.data(), writable_end_size) == 0);
        }
    }

    SyntheticDeviceContext::SyntheticDeviceContext(uint64_t writable_hdr_size, uint64_t rz_size,
                                                   uint64_t writable_end_size)
            : device(nullptr), exit_callback(nullptr) {

        // Create a temporary file
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> distrib(0, 15);

        std::stringstream ss;
        ss << "/tmp/blocks_temp_" << std::hex;
        for (int i = 0; i < 8; ++i) {
            ss << distrib(gen);
        }
        ss << ".img";

        temp_file_path = ss.str();

        // Create and truncate the file
        int fd = open(temp_file_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) {
            throw std::runtime_error("Failed to create temporary file: " + temp_file_path);
        }

        if (ftruncate(fd, writable_hdr_size + writable_end_size) != 0) {
            close(fd);
            unlink(temp_file_path.c_str());
            throw std::runtime_error("Failed to truncate temporary file");
        }

        close(fd);

        // Set up loopback device
        std::string cmd_output;
        FILE *pipe = popen(("losetup -f --show -- " + temp_file_path).c_str(), "r");
        if (!pipe) {
            unlink(temp_file_path.c_str());
            throw std::runtime_error("Failed to execute losetup command");
        }

        char buffer[128];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            cmd_output += buffer;
        }

        int status = pclose(pipe);
        if (status != 0) {
            unlink(temp_file_path.c_str());
            throw std::runtime_error("losetup command failed with status " + std::to_string(status));
        }

        lo_dev_path = cmd_output;
        if (!lo_dev_path.empty() && lo_dev_path.back() == '\n') {
            lo_dev_path.pop_back();
        }

        if (lo_dev_path.empty() || lo_dev_path[0] != '/') {
            unlink(temp_file_path.c_str());
            throw std::runtime_error("Invalid loopback device path: " + lo_dev_path);
        }

        // Generate unique device names
        uuid_t uuid;
        char uuid_str[37];

        uuid_generate(uuid);
        uuid_unparse(uuid, uuid_str);
        rozeros_devname = "rozeros-" + std::string(uuid_str);

        uuid_generate(uuid);
        uuid_unparse(uuid, uuid_str);
        synth_devname = "synthetic-" + std::string(uuid_str);
        synth_devpath = "/dev/mapper/" + synth_devname;

        // Calculate sectors
        uint64_t writable_sectors = bytes_to_sector(writable_hdr_size);
        uint64_t wrend_sectors = bytes_to_sector(writable_end_size);
        uint64_t rz_sectors = bytes_to_sector(rz_size);
        uint64_t wrend_sectors_offset = writable_sectors + rz_sectors;

        // Create the device mapper devices
        std::function<void()> rozeros_exit_callback;
        mk_dm(
                rozeros_devname,
                "0 " + std::to_string(rz_sectors) + " error\n",
                true,
                rozeros_exit_callback
        );

        std::string dm_table_format =
                "0 " + std::to_string(writable_sectors) + " linear " + lo_dev_path + " 0\n" +
                std::to_string(writable_sectors) + " " + std::to_string(rz_sectors) +
                " linear /dev/mapper/" + rozeros_devname + " 0\n";

        if (writable_end_size) {
            dm_table_format +=
                    std::to_string(wrend_sectors_offset) + " " + std::to_string(wrend_sectors) +
                    " linear " + lo_dev_path + " " + std::to_string(writable_sectors) + "\n";
        }

        std::function<void()> synth_exit_callback;
        mk_dm(
                synth_devname,
                dm_table_format,
                false,
                synth_exit_callback
        );

        // Set up the exit callback to clean up everything
        exit_callback = [this, rozeros_exit_callback, synth_exit_callback]() {
            synth_exit_callback();
            rozeros_exit_callback();

            // Detach the loopback device
            std::vector<std::string> losetup_cmd = {"losetup", "-d", lo_dev_path};
            try {
                quiet_call(losetup_cmd);
            } catch (const std::exception &e) {
                std::cerr << "Warning: Failed to detach loopback device: " << e.what() << std::endl;
            }

            // Remove the temporary file
            if (unlink(temp_file_path.c_str()) != 0) {
                std::cerr << "Warning: Failed to remove temporary file: " << temp_file_path << std::endl;
            }
        };

        // Create the synthetic device
        device = std::make_unique<SyntheticDevice>(synth_devpath);
        device->writable_hdr_size = writable_hdr_size;
        device->rz_size = rz_size;
        device->writable_end_size = writable_end_size;
    }

    SyntheticDeviceContext::~SyntheticDeviceContext() {
        cleanup();
    }

    void SyntheticDeviceContext::cleanup() {
        if (exit_callback) {
            exit_callback();
            exit_callback = nullptr;
        }

        // Read the data from the temporary file before it's deleted
        if (device && std::filesystem::exists(temp_file_path)) {
            std::ifstream file(temp_file_path, std::ios::binary);
            if (file) {
                device->data.resize(device->writable_hdr_size + device->writable_end_size);
                file.read(reinterpret_cast<char *>(device->data.data()), device->data.size());
            }
        }
    }

    SyntheticDevice *SyntheticDeviceContext::operator->() {
        return device.get();
    }

    SyntheticDevice &SyntheticDeviceContext::operator*() {
        return *device;
    }

    std::unique_ptr<SyntheticDeviceContext> synth_device(
            uint64_t writable_hdr_size,
            uint64_t rz_size,
            uint64_t writable_end_size) {

        return std::make_unique<SyntheticDeviceContext>(
                writable_hdr_size, rz_size, writable_end_size);
    }

} // namespace blocks
