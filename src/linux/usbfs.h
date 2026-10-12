// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <chrono>
#include <string>

#include "linux/fd.h"
#include "linux/usb_topology.h"
#include "proto/bytes.h"

namespace ironkey {

class ModeSwitchError : public IronKeyError {
public:
    using IronKeyError::IronKeyError;
};

// Raw access to a USB device through /dev/bus/usb/BBB/DDD.
class UsbfsDevice {
public:
    explicit UsbfsDevice(const std::string& node);

    // Standard device-to-host control transfer. Returns the bytes received.
    Bytes control_in(uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                     uint16_t length, unsigned timeout_ms = 5000);

    Bytes device_descriptor();

private:
    std::string node_;
    Fd fd_;
};

// Switches a locked IronKey into HID mode (PROTOCOL.md, Step 1): reads a
// magic sequence of string descriptors, after which the device re-enumerates
// on the same port with a HID interface. Mounted volumes on the device must
// be released first (see release_volumes).
void trigger_hid_mode(const UsbDevice& dev);

// Waits until the device on dev.port exposes a hidraw node, refreshing `dev`.
bool wait_for_hid_mode(UsbDevice& dev, std::chrono::seconds timeout);

}  // namespace ironkey
