cmake_minimum_required(VERSION 3.20)
project(ironkey VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)

set(IRONKEY_WARNINGS -Wall -Wextra -Wpedantic -Wshadow)

# Protocol: no OS dependencies beyond OpenSSL, fully unit-testable.
add_library(ironkey_proto STATIC
    src/proto/bytes.cpp
    src/proto/crypto.cpp
    src/proto/channel.cpp
    src/proto/session.cpp
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
)
target_link_libraries(ironkey_linux PUBLIC ironkey_proto)
target_compile_options(ironkey_linux PRIVATE ${IRONKEY_WARNINGS})

add_executable(ironkey-unlock src/cli/main.cpp)
target_link_libraries(ironkey-unlock PRIVATE ironkey_linux)
target_compile_options(ironkey-unlock PRIVATE ${IRONKEY_WARNINGS})

include(CTest)
if(BUILD_TESTING)
    add_executable(ironkey_tests
        tests/main.cpp
        tests/test_vectors.cpp
        tests/test_simulated_device.cpp
    )
    target_link_libraries(ironkey_tests PRIVATE ironkey_proto)
    target_compile_options(ironkey_tests PRIVATE ${IRONKEY_WARNINGS})
    add_test(NAME ironkey_tests COMMAND ironkey_tests)
endif()
