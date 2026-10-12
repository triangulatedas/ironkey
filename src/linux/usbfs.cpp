// SPDX-License-Identifier: GPL-2.0-only
#include "linux/usbfs.h"

#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>
#include <thread>

namespace ironkey {

namespace {

constexpr uint8_t kEndpointIn = 0x80;
constexpr uint8_t kReqGetDescriptor = 0x06;
constexpr uint8_t kDtDevice = 0x01;
constexpr uint8_t kDtString = 0x03;
constexpr uint16_t kLangUsEnglish = 0x0409;

}  // namespace

UsbfsDevice::UsbfsDevice(const std::string& node)
    : node_(node), fd_(::open(node.c_str(), O_RDWR | O_CLOEXEC))
{
    if (!fd_)
        throw std::system_error(errno, std::generic_category(), "open " + node);
}

Bytes UsbfsDevice::control_in(uint8_t request_type, uint8_t request, uint16_t value,
                              uint16_t index, uint16_t length, unsigned timeout_ms)
{
    Bytes buf(length);
    usbdevfs_ctrltransfer ctrl{};
    ctrl.bRequestType = request_type;
    ctrl.bRequest = request;
    ctrl.wValue = value;
    ctrl.wIndex = index;
    ctrl.wLength = length;
    ctrl.timeout = timeout_ms;
    ctrl.data = buf.data();
    int n = ::ioctl(fd_.get(), USBDEVFS_CONTROL, &ctrl);
    if (n < 0)
        throw std::system_error(errno, std::generic_category(), "control transfer " + node_);
    buf.resize(static_cast<size_t>(n));
    return buf;
}

Bytes UsbfsDevice::device_descriptor()
{
    return control_in(kEndpointIn, kReqGetDescriptor, kDtDevice << 8, 0, 18);
}

void trigger_hid_mode(const UsbDevice& dev)
{
    UsbfsDevice usb(dev.usbfs_node);

    Bytes desc = usb.device_descriptor();
    if (desc.size() < 18)
        throw ModeSwitchError("Could not read device descriptor");
    uint8_t i_serial = desc[16];
    if (i_serial == 0)
        throw ModeSwitchError("Device has no serial number descriptor");

    // Note: the original tool tried to detach usb-storage first, but used an
    // invalid ioctl number, so in practice the driver stays bound. This
    // sequence is what is known to work.
    const uint8_t a = static_cast<uint8_t>(i_serial + 2);
    const uint8_t b = static_cast<uint8_t>(i_serial + 3);
    const uint8_t pattern[] = {a, a, b, b, a, a};

    for (size_t i = 0; i < std::size(pattern); ++i) {
        try {
            usb.control_in(kEndpointIn, kReqGetDescriptor,
                           static_cast<uint16_t>(kDtString << 8 | pattern[i]), kLangUsEnglish, 16);
        } catch (const std::system_error&) {
            // The device drops off the bus as it switches, so failures are
            // expected towards the end of the sequence.
            if (i == std::size(pattern) - 1)
                break;
        }
        if (i + 1 < std::size(pattern))
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool wait_for_hid_mode(UsbDevice& dev, std::chrono::seconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        // The node can appear before udev has applied the uaccess ACL.
        if (refresh(dev) && dev.in_hid_mode() &&
            ::access(dev.hidraw.front().c_str(), R_OK | W_OK) == 0)
            return true;
    }
    return false;
}

}  // namespace ironkey
