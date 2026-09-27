cmake_minimum_required(VERSION 3.15)
project(ciel-mountd C)

# 1. Matches your -std=gnu11 flag
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# 2. Native configuration to generate compile_commands.json automatically
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 3. Matches your CFLAGS += -Wall -Wextra -O2
add_compile_options(-Wall -Wextra -O2)

# 4. Find all packages using PkgConfig (Added libmtp here!)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED dbus-1 fuse3 libusb-1.0 libudev libmtp gio-2.0)

# 5. Matches SRC := $(wildcard src/*.c)
file(GLOB_RECURSE SRC_FILES "src/*.c")

# 6. Define output directories to match your build/bin workspace layout
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# 7. Define executable target
add_executable(ciel-mountd ${SRC_FILES})

# 8. Include header locations
target_include_directories(ciel-mountd PRIVATE include ${DEPS_INCLUDE_DIRS})

# 9. Link all necessary libraries (-ldbus-1, -lfuse3, -lusb-1.0, -ludev, -lmtp)
target_link_libraries(ciel-mountd PRIVATE ${DEPS_LIBRARIES})

# Rename the output binary to match your exact naming convention
set_target_properties(ciel-mountd PROPERTIES OUTPUT_NAME "ciel-mountd")

# 10. Replaces the 'install' target rule matrix
include(GNUInstallDirs)

# Installs binary to /usr/local/sbin/ciel-mountd
install(TARGETS ciel-mountd DESTINATION /usr/local/sbin)

# Installs systemd service file
install(FILES systemd/ciel-mountd.service DESTINATION /etc/systemd/system)
