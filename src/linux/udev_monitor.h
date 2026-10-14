// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <optional>
#include <string>

struct udev;
struct udev_monitor;

namespace ironkey {

struct UsbEvent {
    enum class Action { Add, Remove, Other };
    Action action = Action::Other;
    std::string port;     // sysfs name, e.g. "2-1"
    uint16_t vid = 0;
    std::string product;  // empty on remove
};

// Receives USB device add/remove events from udev, after udev rules
// (including uaccess) have been applied, plus Action::Other events on a
// Kingston drive's port when its disk changes (e.g. unlocked or locked).
class UdevMonitor {
public:
    UdevMonitor();
    ~UdevMonitor();
    UdevMonitor(const UdevMonitor&) = delete;
    UdevMonitor& operator=(const UdevMonitor&) = delete;

    // File descriptor to poll for readability.
    int fd() const;

    // Reads one pending event; nullopt if none was available.
    std::optional<UsbEvent> receive();

private:
    udev* udev_ = nullptr;
    udev_monitor* monitor_ = nullptr;
};

}  // namespace ironkey
