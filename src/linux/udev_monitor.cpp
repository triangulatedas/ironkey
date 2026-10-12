// SPDX-License-Identifier: GPL-2.0-only
#include "linux/udev_monitor.h"

#include <libudev.h>

#include <cstring>
#include <stdexcept>

namespace ironkey {

namespace {

// PRODUCT=951/159d/100 (hex vid/pid/bcdDevice), present on remove too.
uint16_t vid_from_product_property(const char* prop)
{
    if (!prop)
        return 0;
    return static_cast<uint16_t>(std::strtoul(prop, nullptr, 16));
}

}  // namespace

UdevMonitor::UdevMonitor()
{
    udev_ = udev_new();
    if (!udev_)
        throw std::runtime_error("udev_new failed");
    monitor_ = udev_monitor_new_from_netlink(udev_, "udev");
    if (!monitor_) {
        udev_unref(udev_);
        throw std::runtime_error("udev_monitor_new_from_netlink failed");
    }
    udev_monitor_filter_add_match_subsystem_devtype(monitor_, "usb", "usb_device");
    if (udev_monitor_enable_receiving(monitor_) < 0) {
        udev_monitor_unref(monitor_);
        udev_unref(udev_);
        throw std::runtime_error("udev_monitor_enable_receiving failed");
    }
}

UdevMonitor::~UdevMonitor()
{
    udev_monitor_unref(monitor_);
    udev_unref(udev_);
}

int UdevMonitor::fd() const
{
    return udev_monitor_get_fd(monitor_);
}

std::optional<UsbEvent> UdevMonitor::receive()
{
    udev_device* dev = udev_monitor_receive_device(monitor_);
    if (!dev)
        return std::nullopt;

    UsbEvent ev;
    const char* action = udev_device_get_action(dev);
    if (action && std::strcmp(action, "add") == 0)
        ev.action = UsbEvent::Action::Add;
    else if (action && std::strcmp(action, "remove") == 0)
        ev.action = UsbEvent::Action::Remove;

    if (const char* name = udev_device_get_sysname(dev))
        ev.port = name;
    ev.vid = vid_from_product_property(udev_device_get_property_value(dev, "PRODUCT"));
    if (ev.action == UsbEvent::Action::Add)
        if (const char* product = udev_device_get_sysattr_value(dev, "product"))
            ev.product = product;

    udev_device_unref(dev);
    return ev;
}

}  // namespace ironkey
