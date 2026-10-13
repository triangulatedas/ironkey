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

// Reads the drive's pre-login status (GetAreaParams) without disrupting it:
// over the SCSI helper when the drive is locked in normal storage mode, or over
// HID when it is already in HID mode. Never switches a locked drive to HID.
// Returns an Unknown status if neither read is possible.
DriveStatus read_drive_status(const UsbDevice& dev);

// Locates the setcap'd SCSI helper: $IRONKEY_SCSI_HELPER, else next to this
// executable, else "ironkey-scsi-helper" on PATH.
std::string find_scsi_helper();

// True if the replug-free SCSI path can be used right now: the drive is in
// normal storage mode and a harmless FF A0 read succeeds through the helper (so
// the helper exists, is setcap'd, and can reach the device).
bool scsi_usable(const UsbDevice& dev);

// A one-glance description of a device for the agent/tray: enough to tell it
// apart from other connected drives and to decide how to log in, gathered in a
// single non-disruptive pass (see read_device_summary).
struct DeviceSummary {
    std::string name;        // describe(dev): product + S/N + port (always set)
    std::string owner_name;  // owner-profile name if set and readable, else ""
    PasswordSetup setup = PasswordSetup::Unknown;
    std::optional<int> admin_failed_attempts;  // index 0; user counter not exposed
    bool unlocked = false;
    bool status_read = false;  // true if GetAreaParams was actually read
    // Note: there is no reliable "is a drive blank / has no password" signal in
    // the status queries. A never-initialized drive is indistinguishable from a
    // single-password drive that has had one failed login (both read
    // account_flags 0x03 with GetAreaParams[0x0C] = 1), and the vendor's own
    // richer queries carry no extra account state (see PROTOCOL.md). So we do not
    // guess; a UI should simply offer both "unlock" and "set up" for a locked
    // drive. account_flags DOES cleanly identify admin+user (0x07) vs not (0x03).
};

// Reads a DeviceSummary without disrupting a locked drive (SCSI when locked in
// storage mode, HID when already in HID mode; never switches a locked drive to
// HID). `name` and `unlocked` are always filled from the USB topology; the rest
// is best-effort and left at its default when it cannot be read.
DeviceSummary read_device_summary(const UsbDevice& dev);

// Reads the owner profile (incl. password hints) without disrupting a locked
// drive — over SCSI when it is locked in storage mode, HID when already in HID
// mode. Returns an empty Profile when it cannot be read. See read_device_summary.
Profile read_profile_auto(const UsbDevice& dev);

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
// sets a new admin password. Pass a non-empty `user_password` to also provision
// a user account. The drive must be in HID mode; no old password is needed.
// Create a filesystem afterwards with mkfs.
void reset_device_full(UsbDevice& dev, std::string_view new_password, const Profile& profile,
                       const Log& log, bool trace = false, std::string_view user_password = {});

// Soft power-cycle over SCSI (FF 01, NTU_Detach_Attach with Reboot): the drive
// drops off the bus and re-enumerates, applying a new area geometry without a
// physical replug. Waits for it to leave and come back (then lets udev settle)
// and refreshes `dev` to the re-enumerated device. The drive must be in normal
// storage mode with the SCSI helper usable. Returns true if it came back.
bool reenumerate(UsbDevice& dev, const Log& log);

// Which transport reset_device_auto used, and whether the caller must still tell
// the user to replug (the SCSI path power-cycles itself; HID needs one replug).
enum class ResetTransport { Scsi, Hid };
struct ResetOutcome {
    ResetTransport transport = ResetTransport::Hid;
    bool replug_needed = true;
};

// Full reset choosing the transport automatically: the replug-free SCSI path
// when the helper is usable and the drive is in storage mode, else HID.
// `prefer_scsi` forces the choice (nullopt = auto); forcing SCSI when it is not
// usable throws. Wipes the drive (crypto-erase, DESTROYS ALL DATA) and sets the
// admin password, plus the user password when `user_password` is non-empty;
// writes `profile` when it is non-empty. On the SCSI path it then power-cycles
// and waits for the drive to re-enumerate (refreshing `dev`). No old password is
// needed. Create a filesystem on the blank data partition afterwards.
ResetOutcome reset_device_auto(UsbDevice& dev, std::string_view admin_password,
                               std::string_view user_password, const Profile& profile,
                               std::optional<bool> prefer_scsi, const Log& log);

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

// Admin resets the user password (no old user password, no data loss). The drive
// must be in HID mode. Throws WrongPasswordError if the admin password is wrong.
void reset_user_password_device(UsbDevice& dev, std::string_view admin_password,
                                std::string_view new_user_password, const Log& log,
                                bool trace = false);

// Locks an unlocked drive by unmounting it and powering it off, then waits
// for it to leave the bus. Returns false if it was not unlocked.
bool lock_device(UsbDevice& dev, const Log& log);

// Polls until the device shows an unlocked data disk.
bool wait_for_unlocked(UsbDevice& dev, std::chrono::seconds timeout);

// Short human-readable description, e.g. "Locker+50G2 (S/N 80C5…, port 2-1)".
std::string describe(const UsbDevice& dev);

}  // namespace ironkey
