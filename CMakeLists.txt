cmake_minimum_required(VERSION 3.10)
project(blocks VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add compiler warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Find required packages
find_package(CURL REQUIRED)
find_package(nlohmann_json 3.2.0 REQUIRED)  # Found at version 3.10.5 in your output
find_package(PkgConfig REQUIRED)  # Add pkg-config support
pkg_check_modules(UUID REQUIRED uuid)  # Use pkg-config to find libuuid

find_library(PCRECPP_LIBRARY NAMES pcrecpp)
find_path(PCRECPP_INCLUDE_DIR NAMES pcrecpp.h)
include_directories(${PCRECPP_INCLUDE_DIR})

# Source files
set(SOURCES
        block_device.cpp
        filesystem.cpp
        container.cpp
        block_stack.cpp
        synthetic_device.cpp
        lvm_operations.cpp
        bcache_operations.cpp
        resize_operations.cpp
        maintboot_operations.cpp
        main.cpp
)

# Header files
set(HEADERS
        blocks_types.h
        block_device.h
        filesystem.h
        container.h
        block_stack.h
        synthetic_device.h
        lvm_operations.h
        bcache_operations.h
        resize_operations.h
        maintboot_operations.h
)

# Create the executable
add_executable(blocks ${SOURCES} ${HEADERS})

# Include directories
target_include_directories(blocks PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CURL_INCLUDE_DIRS}
        ${UUID_INCLUDE_DIRS}  # Updated to use pkg-config variable
)

# Link libraries
target_link_libraries(blocks PRIVATE
        CURL::libcurl
        nlohmann_json::nlohmann_json
        ${UUID_LIBRARIES}  # Updated to use pkg-config variable
        ${PCRECPP_LIBRARY}
)


# Install target
install(TARGETS blocks
        RUNTIME DESTINATION bin
)

# Install data files
install(FILES maintboot.init
        DESTINATION share/blocks
        PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
)

# Testing
enable_testing()
add_test(NAME blocks_help
        COMMAND blocks --help
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)