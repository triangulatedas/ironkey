cmake_minimum_required(VERSION 3.20)
project(ironkey VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(GNUInstallDirs)

find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
find_package(PkgConfig REQUIRED)
pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

set(IRONKEY_WARNINGS -Wall -Wextra -Wpedantic -Wshadow)

# Protocol: no OS dependencies beyond OpenSSL, fully unit-testable.
add_library(ironkey_proto STATIC
    src/proto/bytes.cpp
    src/proto/crypto.cpp
    src/proto/channel.cpp
    src/proto/session.cpp
    src/proto/tracing_device.cpp
)
target_include_directories(ironkey_proto PUBLIC src)
target_link_libraries(ironkey_proto PUBLIC OpenSSL::Crypto)
target_compile_options(ironkey_proto PRIVATE ${IRONKEY_WARNINGS})

# Linux device access.
add_library(ironkey_linux STATIC
    src/linux/hidraw_device.cpp
    src/linux/usb_topology.cpp
    src/linux/usbfs.cpp
    src/linux/volumes.cpp
    src/linux/device_ops.cpp
    src/linux/scsi_transport.cpp
    src/linux/udev_monitor.cpp
)
target_link_libraries(ironkey_linux PUBLIC ironkey_proto PRIVATE PkgConfig::UDEV PkgConfig::SYSTEMD)
target_compile_options(ironkey_linux PRIVATE ${IRONKEY_WARNINGS})

add_executable(ironkey-unlock src/cli/main.cpp)
target_link_libraries(ironkey-unlock PRIVATE ironkey_linux)
target_compile_options(ironkey-unlock PRIVATE ${IRONKEY_WARNINGS})

add_executable(ironkey-agent
    src/agent/main.cpp
    src/agent/agent.cpp
    src/agent/password_dialog.cpp
    src/agent/notify.cpp
    src/agent/tray.cpp
)
target_link_libraries(ironkey-agent PRIVATE ironkey_linux PkgConfig::SYSTEMD)
target_compile_options(ironkey-agent PRIVATE ${IRONKEY_WARNINGS})

# Tiny privileged helper: only issues SG_IO. Install with
# `setcap cap_sys_rawio+ep` so the unprivileged main tool can send vendor
# commands over SCSI without holding the capability itself.
add_executable(ironkey-scsi-helper src/linux/scsi_helper_main.cpp)
target_compile_options(ironkey-scsi-helper PRIVATE ${IRONKEY_WARNINGS})

install(TARGETS ironkey-unlock ironkey-agent ironkey-scsi-helper
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/70-ironkey.rules DESTINATION lib/udev/rules.d)
install(FILES packaging/ironkey-agent.service DESTINATION lib/systemd/user)

include(CTest)
if(BUILD_TESTING)
    add_executable(ironkey_tests
        tests/main.cpp
        tests/test_vectors.cpp
        tests/test_simulated_device.cpp
        tests/test_login.cpp
    )
    target_link_libraries(ironkey_tests PRIVATE ironkey_linux)
    target_compile_options(ironkey_tests PRIVATE ${IRONKEY_WARNINGS})
    add_test(NAME ironkey_tests COMMAND ironkey_tests)
endif()
