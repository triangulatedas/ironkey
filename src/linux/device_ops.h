// SPDX-License-Identifier: GPL-2.0-only
// High-level operations on one IronKey, shared by the CLI and the agent.
#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "linux/usb_topology.h"
#include "proto/bytes.h"
#include "proto/session.h"

namespace ironkey {

// The current user may not open the device nodes (udev rule missing).
class AccessError : public IronKeyError {
public:
    using IronKeyError::IronKeyError;
};

// The drive did not unlock. Either it reported a wrong password, or it
// accepted the sequence but no data disk appeared (a drive that has not been
// set up behaves that way).
class NotUnlockedError : public IronKeyError {
public:
    NotUnlockedError(const std::string& msg, bool wrong_password_,
                     std::optional<int> failed_attempts_ = std::nullopt)
        : IronKeyError(msg), wrong_password(wrong_password_), failed_attempts(failed_attempts_)
    {
    }
    bool wrong_password;
    // Consecutive failed logins as reported by the drive, if it could be read.
    std::optional<int> failed_attempts;
};

// The device disappeared (unplugged) during an operation.
class DeviceGoneError : public IronKeyError {
public:
    using IronKeyError::IronKeyError;
};

using Log = std::function<void(const std::string&)>;

// Throws AccessError unless the raw USB node can be opened read-write.
void check_access(const UsbDevice& dev);

// Brings a locked device into HID mode, releasing its CD-ROM first.
// No-op if it is already in HID mode. Refreshes `dev`.
void ensure_hid_mode(UsbDevice& dev, const Log& log);

// Unlocks a device in HID mode and waits for its data disk to appear.
// Refreshes `dev`. Throws NotUnlockedError if no data disk appears.
// With `trace`, every HID exchange is written to `log` (secrets redacted).
void unlock_device(UsbDevice& dev, std::string_view password, Account account, const Log& log,
                   bool trace = false);

// Reads the drive's consecutive failed-login count for `account`
// (GetAreaParams), which needs HID mode but no session or password.
// nullopt if it cannot be read, or its location is not known (user account).
std::optional<int> read_failed_attempts(const UsbDevice& dev, Account account);

// What happens after too many failures on `account`.
std::string lockout_consequence(Account account);

// E.g. "2 failed admin logins since the last successful one; 8 left before the
// drive erases itself", or "" for 0.
std::string failed_attempts_warning(int failed, Account account);

// Locks an unlocked drive by unmounting it and powering it off, then waits
// for it to leave the bus. Returns false if it was not unlocked.
bool lock_device(UsbDevice& dev, const Log& log);

// Polls until the device shows an unlocked data disk.
bool wait_for_unlocked(UsbDevice& dev, std::chrono::seconds timeout);

// Short human-readable description, e.g. "Locker+50G2 (S/N 80C5…, port 2-1)".
std::string describe(const UsbDevice& dev);

}  // namespace ironkey
