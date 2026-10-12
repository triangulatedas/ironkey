// SPDX-License-Identifier: GPL-2.0-only
#include "linux/device_ops.h"

#include <unistd.h>

#include <algorithm>
#include <thread>

#include "linux/hidraw_device.h"
#include "linux/usbfs.h"
#include "linux/volumes.h"
#include "proto/session.h"
#include "proto/tracing_device.h"

namespace ironkey {

namespace {

bool can_open(const std::string& node)
{
    return ::access(node.c_str(), R_OK | W_OK) == 0;
}

}  // namespace

void check_access(const UsbDevice& dev)
{
    if (!can_open(dev.usbfs_node))
        throw AccessError("No permission to open " + dev.usbfs_node +
                          ". Is the IronKey udev rule installed? Replug the drive after "
                          "installing it.");
}

void ensure_hid_mode(UsbDevice& dev, const Log& log)
{
    if (dev.in_hid_mode())
        return;
    check_access(dev);

    for (const auto& problem : release_volumes(dev))
        log("Warning: " + problem);

    log("Switching " + dev.port + " to HID mode");
    trigger_hid_mode(dev);
    if (!wait_for_hid_mode(dev, std::chrono::seconds(10))) {
        if (!refresh(dev))
            throw DeviceGoneError("Device on port " + dev.port + " was removed");
        throw ModeSwitchError("Device did not come back in HID mode. Try replugging it.");
    }
    log("HID mode: " + dev.hidraw.front());
}

void unlock_device(UsbDevice& dev, std::string_view password, Account account, const Log& log,
                   bool trace)
{
    if (password.size() > kMaxPasswordBytes)
        throw UnlockError("Password longer than 16 bytes. Passphrase mode (10-64 characters) "
                          "is not supported yet");
    if (!dev.in_hid_mode())
        throw IronKeyError("Device is not in HID mode");

    try {
        HidrawDevice hid(dev.hidraw.front());
        TracingHidDevice traced(hid, log);
        unlock(trace ? static_cast<HidDevice&>(traced) : hid, password, account);
    } catch (const WrongPasswordError&) {
        auto failed = read_failed_attempts(dev, account);
        std::string msg = std::string("Wrong ") + to_string(account) + " password";
        if (failed)
            msg += " (" + failed_attempts_warning(*failed, account) + ")";
        else
            msg += ". " + lockout_consequence(account);
        throw NotUnlockedError(msg, true, failed);
    }

    // The device reports status OK even for a wrong password, so the only
    // reliable signal is whether the data disk appears.
    log("Unlock sequence sent, waiting for data disk");
    if (!wait_for_unlocked(dev, std::chrono::seconds(15))) {
        if (!refresh(dev))
            throw DeviceGoneError("Device on port " + dev.port + " was removed");
        throw NotUnlockedError(
            "The drive accepted the password but did not unlock. It may not be set up yet",
            false);
    }
}

std::optional<int> read_failed_attempts(const UsbDevice& dev, Account account)
{
    // Only the admin counter's location is known so far (PROTOCOL.md).
    if (account != Account::Admin || !dev.in_hid_mode())
        return std::nullopt;
    try {
        HidrawDevice hid(dev.hidraw.front());
        Channel ch(hid);
        ch.drain();
        return read_area_params(ch).admin_failed_attempts;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string lockout_consequence(Account account)
{
    std::string n = std::to_string(kDefaultMaxFailedAttempts);
    if (account == Account::User)
        return "User login locks after " + n +
               " failed attempts in a row; the admin can reset it";
    return "The drive erases itself after " + n + " failed attempts in a row";
}

std::string failed_attempts_warning(int failed, Account account)
{
    if (failed <= 0)
        return {};
    int left = std::max(kDefaultMaxFailedAttempts - failed, 0);
    return std::to_string(failed) + " failed " + to_string(account) +
           (failed == 1 ? " login" : " logins") + " since the last successful one; " +
           std::to_string(left) + " left before " +
           (account == Account::User ? "user login locks" : "the drive erases itself") +
           " (default limit " + std::to_string(kDefaultMaxFailedAttempts) + ")";
}

bool lock_device(UsbDevice& dev, const Log& log)
{
    if (!dev.is_unlocked())
        return false;
    log("Unmounting and powering off " + describe(dev));
    power_off_drive(dev);

    // udisks detaches the USB device; wait until it is gone or no longer
    // shows an unlocked disk.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!refresh(dev) || !dev.is_unlocked())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    throw IronKeyError("Drive was powered off but is still visible");
}

bool wait_for_unlocked(UsbDevice& dev, std::chrono::seconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (refresh(dev) && dev.is_unlocked())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return false;
}

std::string describe(const UsbDevice& dev)
{
    std::string s = dev.product.empty() ? "IronKey" : dev.product;
    s += " (";
    if (!dev.serial.empty())
        s += "S/N " + dev.serial + ", ";
    s += "port " + dev.port + ")";
    return s;
}

}  // namespace ironkey
