// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ironkey {

constexpr uint16_t kKingstonVid = 0x0951;

struct BlockDevice {
    std::string node;     // /dev/sdX or /dev/srN
    uint64_t size_bytes;  // 0 for a locked data LUN or an empty CD drive
};

// A Kingston USB device and the device nodes the kernel created for it.
// Devices are identified by their USB port path (e.g. "2-1.4"), which stays
// the same when the IronKey re-enumerates with a new PID after the HID mode
// switch or after unlocking.
struct UsbDevice {
    std::string port;
    std::filesystem::path sysfs;
    uint16_t vid = 0;
    uint16_t pid = 0;
    std::string manufacturer;
    std::string product;
    std::string serial;
    std::string usbfs_node;           // /dev/bus/usb/BBB/DDD
    std::vector<std::string> hidraw;  // /dev/hidrawN
    std::vector<BlockDevice> disks;   // sd*
    std::vector<BlockDevice> cdroms;  // sr*

    bool is_ironkey() const;
    bool in_hid_mode() const { return !hidraw.empty(); }
    // True when a data disk with non-zero size is visible.
    bool is_unlocked() const;
};

// All connected Kingston USB devices (IronKey or not), sorted by port.
std::vector<UsbDevice> find_kingston_devices();

// Re-reads one device by port. Returns false if nothing is on that port.
bool refresh(UsbDevice& dev);

}  // namespace ironkey
