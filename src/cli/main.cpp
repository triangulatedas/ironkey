// SPDX-License-Identifier: GPL-2.0-only
// ironkey-unlock: command-line front end.
#include <unistd.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "linux/hidraw_device.h"
#include "linux/usb_topology.h"
#include "linux/usbfs.h"
#include "linux/volumes.h"
#include "proto/session.h"

using namespace ironkey;

namespace {

const char* access_label(const std::string& node)
{
    return ::access(node.c_str(), R_OK | W_OK) == 0 ? "rw" : "no access";
}

std::string human_size(uint64_t bytes)
{
    char buf[32];
    if (bytes == 0)
        return "empty";
    std::snprintf(buf, sizeof buf, "%.1f GiB", static_cast<double>(bytes) / (1ULL << 30));
    return buf;
}

int cmd_list()
{
    auto devices = find_kingston_devices();
    if (devices.empty()) {
        std::puts("No Kingston USB devices found.");
        return 1;
    }
    for (const auto& d : devices) {
        const char* state = !d.is_ironkey() ? "not an IronKey"
                            : d.is_unlocked() ? "unlocked"
                            : d.in_hid_mode() ? "locked, HID mode"
                                              : "locked";
        std::printf("port %s  %04x:%04x  %s  S/N %s  [%s]\n", d.port.c_str(), d.vid, d.pid,
                    d.product.c_str(), d.serial.empty() ? "-" : d.serial.c_str(), state);
        std::printf("    usb     %s (%s)\n", d.usbfs_node.c_str(), access_label(d.usbfs_node));
        for (const auto& h : d.hidraw)
            std::printf("    hidraw  %s (%s)\n", h.c_str(), access_label(h));
        for (const auto& b : d.cdroms)
            std::printf("    cdrom   %s (%s)\n", b.node.c_str(), human_size(b.size_bytes).c_str());
        for (const auto& b : d.disks)
            std::printf("    disk    %s (%s)\n", b.node.c_str(), human_size(b.size_bytes).c_str());
    }
    return 0;
}

// Picks the IronKey on `port`, or the only IronKey if no port is given.
bool select_device(const std::string& port, UsbDevice& out)
{
    std::vector<UsbDevice> keys;
    for (auto& d : find_kingston_devices())
        if (d.is_ironkey() && (port.empty() || d.port == port))
            keys.push_back(std::move(d));
    if (keys.empty()) {
        std::fprintf(stderr, port.empty() ? "No IronKey found.\n" : "No IronKey on port %s.\n",
                     port.c_str());
        return false;
    }
    if (keys.size() > 1) {
        std::fprintf(stderr, "Several IronKeys connected; choose one with --device PORT:\n");
        for (const auto& d : keys)
            std::fprintf(stderr, "  %s  %s  S/N %s\n", d.port.c_str(), d.product.c_str(),
                         d.serial.c_str());
        return false;
    }
    out = std::move(keys.front());
    return true;
}

// Brings the device into HID mode if it is not already there.
bool ensure_hid_mode(UsbDevice& dev)
{
    if (dev.in_hid_mode()) {
        std::printf("Port %s already in HID mode (%s)\n", dev.port.c_str(), dev.hidraw.front().c_str());
        return true;
    }
    // Check access before touching any mounts.
    if (::access(dev.usbfs_node.c_str(), R_OK | W_OK) != 0) {
        std::fprintf(stderr,
                     "No access to %s. Install packaging/70-ironkey.rules into "
                     "/etc/udev/rules.d and replug the device (or run as root).\n",
                     dev.usbfs_node.c_str());
        return false;
    }
    for (const auto& problem : release_volumes(dev))
        std::fprintf(stderr, "Warning: %s\n", problem.c_str());

    std::printf("Switching port %s to HID mode...\n", dev.port.c_str());
    trigger_hid_mode(dev);
    if (!wait_for_hid_mode(dev, std::chrono::seconds(10))) {
        std::fprintf(stderr, "Device did not come back in HID mode. Try replugging it.\n");
        return false;
    }
    std::printf("HID mode: %04x:%04x, %s\n", dev.vid, dev.pid, dev.hidraw.front().c_str());
    return true;
}

int cmd_hid_mode(const std::string& port)
{
    UsbDevice dev;
    if (!select_device(port, dev))
        return 1;
    return ensure_hid_mode(dev) ? 0 : 1;
}

// Diagnostic: RSA key exchange only. Sends no password and changes nothing.
int cmd_handshake(const std::string& port)
{
    UsbDevice dev;
    if (!select_device(port, dev) || !ensure_hid_mode(dev))
        return 1;

    HidrawDevice hid(dev.hidraw.front());
    Channel ch(hid);
    SystemRandom rng;
    RsaKey host = RsaKey::generate512();
    Key128 shared = rsa_handshake(ch, rng, host);
    secure_wipe(shared.data(), shared.size());
    std::puts("Handshake OK: session key established (FF 81..88).");
    return 0;
}

void usage()
{
    std::puts("Usage: ironkey-unlock list\n"
              "       ironkey-unlock hid-mode  [--device PORT]\n"
              "       ironkey-unlock handshake [--device PORT]");
}

}  // namespace

int main(int argc, char** argv)
{
    std::string_view cmd = argc > 1 ? argv[1] : "list";
    std::string port;
    for (int i = 2; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--device" && i + 1 < argc) {
            port = argv[++i];
        } else {
            usage();
            return 2;
        }
    }

    try {
        if (cmd == "list")
            return cmd_list();
        if (cmd == "hid-mode")
            return cmd_hid_mode(port);
        if (cmd == "handshake")
            return cmd_handshake(port);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
    usage();
    return 2;
}
