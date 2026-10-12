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

enum class PasswordSetup {
    Unknown,
    Single,        // one password (index 0); also how a drive that is not set up reads
    AdminAndUser,  // admin (index 0) and user (index 1) passwords
};

// What the drive reports before login (GetAreaParams). Needs HID mode but no
// session or password, and does not use up an attempt.
struct DriveStatus {
    PasswordSetup setup = PasswordSetup::Unknown;
    std::optional<int> admin_failed_attempts;  // index 0; the user counter is not known
};

// Returns an Unknown status if the drive is not in HID mode or can't be read.
DriveStatus read_drive_status(const UsbDevice& dev);

// The account to log in with and how to talk about it.
struct Login {
    Account account = Account::Admin;
    PasswordSetup setup = PasswordSetup::Unknown;

    // "user password", "admin password", or just "password" on a
    // single-password drive.
    std::string password_name() const;
    // What happens after too many failures in a row.
    std::string lockout_consequence() const;
    // The failed-login count for this account, if the drive exposes it.
    std::optional<int> failed_attempts(const DriveStatus& status) const;
    // E.g. "2 failed logins since the last successful one; 8 left before the
    // drive erases itself (default limit 10)", or "" for 0.
    std::string failed_warning(int failed) const;
};

// Picks the account: `requested` if given, otherwise the user password on
// admin+user drives (as the vendor app does) and index 0 otherwise. Throws
// IronKeyError if the user account is requested on a single-password drive.
Login choose_login(const DriveStatus& status, std::optional<Account> requested);

// The drive's RSA-512 handshake lets a passive USB sniffer recover the
// password with effort (see README "Security note"). Any command that sets or
// sends a password on an untrusted host should show this first. Returned as
// plain lines so both the CLI and a dialog can present it.
extern const char* const kUntrustedHostWarning;

// Unlocks a device in HID mode and waits for its data disk to appear.
// Refreshes `dev`. Throws NotUnlockedError if no data disk appears.
// With `trace`, every HID exchange is written to `log` (secrets redacted).
void unlock_device(UsbDevice& dev, std::string_view password, const Login& login,
                   const Log& log, bool trace = false);

// Sets a password with no prior password (after a re-key), the drive being in
// HID mode. `login.account` picks admin (index 0) or user (index 1).
void set_password_device(UsbDevice& dev, std::string_view new_password, const Login& login,
                         const Log& log, bool trace = false);

// Full factory reset: wipes the drive (crypto-erase, DESTROYS ALL DATA) and
// sets a new single/admin password. The drive must be in HID mode; no old
// password is needed. Create a filesystem afterwards with mkfs.
void reset_device_full(UsbDevice& dev, std::string_view new_password, const Profile& profile,
                       const Log& log, bool trace = false);

// Reads the owner profile (name/company/description/hints) without unlocking.
// The drive must be in HID mode.
Profile read_profile_device(UsbDevice& dev, const Log& log, bool trace = false);

// Re-keys the data area (the crypto-erase step of reset/initialize). Reads the
// area size, runs a handshake, and sends FF 8B/8F/A2. Returns the FF A2 status
// (0 = accepted). With commit=false it stops before the FF 89 commit so
// nothing persists — use it to probe whether the token format is right. The
// drive must be in HID mode; no password is needed.
uint16_t rekey_area_probe(UsbDevice& dev, const Log& log, bool commit, bool trace = false);

// Changes a password in place (the drive must be in HID mode). The device
// rejects a wrong old password with no change (WrongPasswordError). Does not
// unlock the drive. `login.account` picks admin (FF A6) or user (FF A8).
void change_password_device(UsbDevice& dev, std::string_view old_password,
                            std::string_view new_password, const Login& login, const Log& log,
                            bool trace = false);

// Locks an unlocked drive by unmounting it and powering it off, then waits
// for it to leave the bus. Returns false if it was not unlocked.
bool lock_device(UsbDevice& dev, const Log& log);

// Polls until the device shows an unlocked data disk.
bool wait_for_unlocked(UsbDevice& dev, std::chrono::seconds timeout);

// Short human-readable description, e.g. "Locker+50G2 (S/N 80C5…, port 2-1)".
std::string describe(const UsbDevice& dev);

}  // namespace ironkey
