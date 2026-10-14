cmake_minimum_required(VERSION 3.20)
project(ironkey VERSION 0.1.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(GNUInstallDirs)

# Package version = the project version (above) plus the git commit count as the
# Debian revision, with the short SHA for traceability. Falls back cleanly in a
# tarball with no git.
set(IRONKEY_GIT_COUNT 0)
set(IRONKEY_GIT_SHA "nogit")
find_package(Git QUIET)
if(Git_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE IRONKEY_GIT_COUNT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE IRONKEY_GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(NOT IRONKEY_GIT_COUNT)
    set(IRONKEY_GIT_COUNT 0)
endif()
message(STATUS "ironkey version: ${PROJECT_VERSION}-${IRONKEY_GIT_COUNT} (g${IRONKEY_GIT_SHA})")

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
    src/agent/wizard.cpp
)
target_link_libraries(ironkey-agent PRIVATE ironkey_linux PkgConfig::SYSTEMD)
target_compile_options(ironkey-agent PRIVATE ${IRONKEY_WARNINGS})

# Optional GTK3 front-end (ironkey-gui): richer unlock dialog and setup wizard.
# Built only when GTK3 is available; the CLI and agent never link it. Without
# it the agent still runs the tray, but cannot show its dialogs.
pkg_check_modules(GTK3 IMPORTED_TARGET gtk+-3.0)
if(GTK3_FOUND)
    add_executable(ironkey-gui src/gui/ironkey_gui.cpp)
    target_link_libraries(ironkey-gui PRIVATE PkgConfig::GTK3)
    target_compile_options(ironkey-gui PRIVATE -Wall -Wextra)
    install(TARGETS ironkey-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT gui)
    message(STATUS "ironkey-gui: building GTK3 front-end (${GTK3_VERSION})")
else()
    message(STATUS "ironkey-gui: GTK3 not found; the agent will have no dialogs")
endif()

# Tiny privileged helper: only issues SG_IO. Install with
# `setcap cap_sys_rawio+ep` so the unprivileged main tool can send vendor
# commands over SCSI without holding the capability itself.
add_executable(ironkey-scsi-helper src/linux/scsi_helper_main.cpp)
target_compile_options(ironkey-scsi-helper PRIVATE ${IRONKEY_WARNINGS})

install(TARGETS ironkey-unlock ironkey-agent ironkey-scsi-helper
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT core)
install(FILES packaging/70-ironkey.rules DESTINATION lib/udev/rules.d COMPONENT core)
install(FILES packaging/ironkey-agent.service DESTINATION lib/systemd/user COMPONENT core)

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

# ---- Packaging (CPack -> .deb) ---------------------------------------------
# Two packages from one build: `ironkey` (CLI + agent + scsi-helper, udev rule,
# user service) and `ironkey-gui` (the optional GTK front-end). Build the .deb(s)
# with `cpack -G DEB` in the build dir.
if(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "DEB")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_DEB_COMPONENT_INSTALL ON)
    set(CPACK_COMPONENTS_ALL core)
    if(GTK3_FOUND)
        list(APPEND CPACK_COMPONENTS_ALL gui)
    endif()

    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_DEBIAN_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_DEBIAN_PACKAGE_RELEASE "${IRONKEY_GIT_COUNT}")  # git commit count
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)                   # name_ver-rel_arch.deb
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)                    # auto lib deps (needs dpkg-dev)
    set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE
        "https://github.com/wltechblog/ironkey-unlocker")
    set(IRONKEY_MAINTAINER "G <geir.rastad@dibberfamily.com>" CACHE STRING "Package Maintainer field")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${IRONKEY_MAINTAINER}")

    # ironkey (core)
    set(CPACK_DEBIAN_CORE_PACKAGE_NAME "ironkey")
    set(CPACK_COMPONENT_CORE_DESCRIPTION
        "Unlock and manage Kingston IronKey Locker+ G2 drives on Linux")
    set(CPACK_DEBIAN_CORE_PACKAGE_DEPENDS "udisks2, eject")   # + shlibs auto-added
    set(CPACK_DEBIAN_CORE_PACKAGE_RECOMMENDS "ironkey-gui")
    set(CPACK_DEBIAN_CORE_PACKAGE_CONTROL_EXTRA
        "${CMAKE_SOURCE_DIR}/packaging/deb/postinst;${CMAKE_SOURCE_DIR}/packaging/deb/postrm")

    # ironkey-gui (GTK front-end)
    set(CPACK_DEBIAN_GUI_PACKAGE_NAME "ironkey-gui")
    set(CPACK_COMPONENT_GUI_DESCRIPTION
        "GTK front-end (unlock dialog and setup wizard) for ironkey")
    set(CPACK_DEBIAN_GUI_PACKAGE_DEPENDS "ironkey (>= ${PROJECT_VERSION})")  # + shlibs (libgtk-3-0)

    include(CPack)
endif()
