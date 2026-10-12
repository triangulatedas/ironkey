// SPDX-License-Identifier: GPL-2.0-only
#include "linux/usb_topology.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>

namespace ironkey {

namespace fs = std::filesystem;

namespace {

const fs::path kUsbDevices = "/sys/bus/usb/devices";

std::string read_attr(const fs::path& path)
{
    std::ifstream in(path);
    std::string value;
    std::getline(in, value);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}

uint16_t read_hex16(const fs::path& path)
{
    std::string s = read_attr(path);
    return s.empty() ? 0 : static_cast<uint16_t>(std::stoul(s, nullptr, 16));
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// True if `child` is `parent` or lies below it in the sysfs device tree.
bool is_below(const fs::path& child, const fs::path& parent)
{
    auto [p, c] = std::mismatch(parent.begin(), parent.end(), child.begin(), child.end());
    return p == parent.end();
}

fs::path resolve(const fs::path& p)
{
    std::error_code ec;
    fs::path r = fs::canonical(p, ec);
    return ec ? fs::path() : r;
}

void collect_nodes(UsbDevice& dev)
{
    dev.hidraw.clear();
    dev.disks.clear();
    dev.cdroms.clear();
    std::error_code ec;

    for (const auto& entry : fs::directory_iterator("/sys/class/hidraw", ec)) {
        if (is_below(resolve(entry.path() / "device"), dev.sysfs))
            dev.hidraw.push_back("/dev/" + entry.path().filename().string());
    }

    for (const auto& entry : fs::directory_iterator("/sys/block", ec)) {
        std::string name = entry.path().filename().string();
        bool disk = name.starts_with("sd");
        bool cdrom = name.starts_with("sr");
        if (!(disk || cdrom) || !is_below(resolve(entry.path()), dev.sysfs))
            continue;
        std::string sectors = read_attr(entry.path() / "size");
        uint64_t size = sectors.empty() ? 0 : std::stoull(sectors) * 512;
        (disk ? dev.disks : dev.cdroms).push_back({"/dev/" + name, size});
    }

    std::sort(dev.hidraw.begin(), dev.hidraw.end());
}

bool load(const fs::path& sysfs, UsbDevice& dev)
{
    if (!fs::exists(sysfs / "idVendor"))
        return false;
    dev.port = sysfs.filename().string();
    dev.sysfs = resolve(sysfs);
    dev.vid = read_hex16(sysfs / "idVendor");
    dev.pid = read_hex16(sysfs / "idProduct");
    dev.manufacturer = read_attr(sysfs / "manufacturer");
    dev.product = read_attr(sysfs / "product");
    dev.serial = read_attr(sysfs / "serial");

    char node[32];
    std::snprintf(node, sizeof node, "/dev/bus/usb/%03d/%03d",
                  std::atoi(read_attr(sysfs / "busnum").c_str()),
                  std::atoi(read_attr(sysfs / "devnum").c_str()));
    dev.usbfs_node = node;

    collect_nodes(dev);
    return true;
}

}  // namespace

bool UsbDevice::is_ironkey() const
{
    if (vid != kKingstonVid)
        return false;
    std::string p = lower(product);
    return p.find("ironkey") != std::string::npos || p.find("locker") != std::string::npos;
}

bool UsbDevice::is_unlocked() const
{
    return std::any_of(disks.begin(), disks.end(),
                       [](const BlockDevice& d) { return d.size_bytes > 1'000'000; });
}

std::vector<UsbDevice> find_kingston_devices()
{
    std::vector<UsbDevice> result;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(kUsbDevices, ec)) {
        std::string name = entry.path().filename().string();
        // Skip interfaces ("1-2:1.0") and root hubs ("usb1").
        if (name.find(':') != std::string::npos || name.starts_with("usb"))
            continue;
        UsbDevice dev;
        if (load(entry.path(), dev) && dev.vid == kKingstonVid)
            result.push_back(std::move(dev));
    }
    std::sort(result.begin(), result.end(),
              [](const UsbDevice& a, const UsbDevice& b) { return a.port < b.port; });
    return result;
}

bool refresh(UsbDevice& dev)
{
    UsbDevice fresh;
    if (!load(kUsbDevices / dev.port, fresh))
        return false;
    dev = std::move(fresh);
    return true;
}

}  // namespace ironkey
