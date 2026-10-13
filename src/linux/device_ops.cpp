// SPDX-License-Identifier: GPL-2.0-only
#include "linux/device_ops.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <thread>

#include "linux/hidraw_device.h"
#include "linux/scsi_transport.h"
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

// Re-finds a single IronKey by USB port (empty port = any). Used to wait for a
// drive to reappear after a soft power-cycle.
bool find_ironkey_on_port(const std::string& port, UsbDevice& out)
{
    for (auto& d : find_kingston_devices())
        if (d.is_ironkey() && (port.empty() || d.port == port)) {
            out = std::move(d);
            return true;
        }
    return false;
}

// Opens a non-disruptive read channel to the drive and runs `fn(ch)`: over HID
// when the drive is already in HID mode, else over the SCSI helper when it is in
// normal storage mode. Never switches a locked drive to HID. Returns true only
// if a channel was available and `fn` ran to completion without throwing.
template <class Fn>
bool on_read_channel(const UsbDevice& dev, Fn&& fn)
{
    try {
        if (dev.in_hid_mode()) {
            HidrawDevice hid(dev.hidraw.front());
            Channel ch(hid);
            ch.drain();
            fn(ch);
            return true;
        }
        if (!dev.disks.empty()) {
            ScsiTransport st(find_scsi_helper(), dev.disks.front().node);
            Channel ch(st);
            ch.drain();
            fn(ch);
            return true;
        }
    } catch (const std::exception&) {
    }
    return false;
}

}  // namespace

std::string find_scsi_helper()
{
    if (const char* e = std::getenv("IRONKEY_SCSI_HELPER"))
        return e;
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = '\0';
        std::string p(buf);
        auto slash = p.rfind('/');
        if (slash != std::string::npos)
            return p.substr(0, slash + 1) + "ironkey-scsi-helper";
    }
    return "ironkey-scsi-helper";
}

bool scsi_usable(const UsbDevice& dev)
{
    if (dev.disks.empty())
        return false;
    try {
        ScsiTransport st(find_scsi_helper(), dev.disks.front().node);
        Channel ch(st);
        ch.drain();
        read_area_params(ch);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

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

void unlock_device(UsbDevice& dev, std::string_view password, const Login& login,
                   const Log& log, bool trace)
{
    if (password.size() > kMaxPassphraseBytes)
        throw UnlockError("Password longer than 64 characters");
    if (!dev.in_hid_mode())
        throw IronKeyError("Device is not in HID mode");

    try {
        HidrawDevice hid(dev.hidraw.front());
        TracingHidDevice traced(hid, log);
        Channel ch(trace ? static_cast<HidDevice&>(traced) : hid);
        unlock(ch, password, login.account);
    } catch (const WrongPasswordError&) {
        auto failed = login.failed_attempts(read_drive_status(dev));
        std::string msg = "Wrong " + login.password_name();
        if (failed)
            msg += " (" + login.failed_warning(*failed) + ")";
        else
            msg += ". " + login.lockout_consequence();
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

void set_password_device(UsbDevice& dev, std::string_view new_password, const Login& login,
                         const Log& log, bool trace)
{
    if (!dev.in_hid_mode())
        throw IronKeyError("Device is not in HID mode");
    HidrawDevice hid(dev.hidraw.front());
    TracingHidDevice traced(hid, log);
    Channel ch(trace ? static_cast<HidDevice&>(traced) : hid);
    set_password(ch, new_password, login.account);
    log("Password set.");
}

void reset_device_full(UsbDevice& dev, std::string_view new_password, const Profile& profile,
                       const Log& log, bool trace, std::string_view user_password)
{
    if (!dev.in_hid_mode())
        throw IronKeyError("Device is not in HID mode");
    HidrawDevice hid(dev.hidraw.front());
    TracingHidDevice traced(hid, log);
    Channel ch(trace ? static_cast<HidDevice&>(traced) : hid);
    // Reset sets the admin password (index 0), and the user password (index 1)
    // when one is given.
    reset_device(ch, new_password, user_password);
    log(user_password.empty() ? "Reset complete: area wiped and new password set."
                              : "Reset complete: area wiped, admin and user passwords set.");
    if (!profile.empty()) {
        write_profile(ch, profile);
        log("Owner info written.");
    }
}

bool reenumerate(UsbDevice& dev, const Log& log)
{
    if (dev.disks.empty())
        throw IronKeyError("Cannot re-enumerate: the drive is not in normal storage mode");
    const std::string port = dev.port;
    {
        ScsiTransport st(find_scsi_helper(), dev.disks.front().node);
        Channel ch(st);
        // FF 01 (NTU_Detach_Attach, Reboot=1): the device drops off the bus, so
        // this command returns no status; the exception is expected.
        try {
            Bytes ff01 = {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1};
            ch.command(0x01, 0x01, Direction::Write, ff01, 12, {0, 0, 0, 0, 0, 0, 0, 0});
        } catch (const std::exception&) {
        }
    }
    log("Re-enumerating the drive (soft power-cycle)...");
    // Wait for the device to drop off the bus and come back, then let udev settle
    // (node numbers and access ACLs change on re-enumeration).
    UsbDevice back;
    bool went_away = false, came_back = false;
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        bool present = find_ironkey_on_port(port, back);
        if (!present)
            went_away = true;
        else if (went_away) {
            came_back = true;
            break;
        }
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (came_back)
        dev = std::move(back);
    return came_back;
}

ResetOutcome reset_device_auto(UsbDevice& dev, std::string_view admin_password,
                               std::string_view user_password, const Profile& profile,
                               std::optional<bool> prefer_scsi, const Log& log)
{
    bool use_scsi;
    if (prefer_scsi.has_value()) {
        use_scsi = *prefer_scsi;
        if (use_scsi && !scsi_usable(dev))
            throw IronKeyError("SCSI reset is not available: the drive must be in normal storage "
                               "mode and ironkey-scsi-helper must be installed with "
                               "`setcap cap_sys_rawio+ep`.");
    } else {
        use_scsi = scsi_usable(dev);
    }

    if (use_scsi) {
        log("Using the SCSI path (no replug needed).");
        {
            ScsiTransport st(find_scsi_helper(), dev.disks.front().node);
            Channel ch(st);
            reset_device(ch, admin_password, user_password);
            log(user_password.empty() ? "Reset complete: area wiped and new password set."
                                      : "Reset complete: area wiped, admin and user passwords set.");
            if (!profile.empty()) {
                write_profile(ch, profile);
                log("Owner info written.");
            }
        }
        reenumerate(dev, log);
        return {ResetTransport::Scsi, /*replug_needed=*/false};
    }

    log("Using the HID path (one replug needed after reset).");
    check_access(dev);
    ensure_hid_mode(dev, log);
    reset_device_full(dev, admin_password, profile, log, /*trace=*/false, user_password);
    return {ResetTransport::Hid, /*replug_needed=*/true};
}

Profile read_profile_auto(const UsbDevice& dev)
{
    Profile p;
    on_read_channel(dev, [&](Channel& ch) { p = read_profile(ch); });
    return p;
}

Profile read_profile_device(UsbDevice& dev, const Log& log, bool trace)
{
    if (!dev.in_hid_mode())
        throw IronKeyError("Device is not in HID mode");
    HidrawDevice hid(dev.hidraw.front());
    TracingHidDevice traced(hid, log);
    Channel ch(trace ? static_cast<HidDevice&>(traced) : hid);
    return read_profile(ch);
}

uint16_t rekey_area_probe(UsbDevice& dev, const Log& log, bool commit, bool trace)
{
    if (!dev.in_hid_mode())
        throw IronKeyError("Device is not in HID mode");

    HidrawDevice hid(dev.hidraw.front());
    TracingHidDevice traced(hid, log);
    HidDevice& io = trace ? static_cast<HidDevice&>(traced) : hid;

    Channel ch(io);
    SystemRandom rng;

    // The area size to re-key comes from the drive's own GetAreaParams.
    uint32_t sectors;
    {
        Channel probe(hid);
        probe.drain();
        sectors = read_area_params(probe).data_sectors;
    }
    if (sectors == 0)
        throw IronKeyError("Could not read the data area size (FF A0)");
    log("Data area: " + std::to_string(sectors) + " sectors (" +
        std::to_string(uint64_t(sectors) * 512 / (1u << 30)) + " GiB)");

    RsaKey host = RsaKey::generate512();
    Key128 shared = rsa_handshake(ch, rng, host);
    uint16_t status = send_configure_area(ch, shared, sectors, rng, commit);
    secure_wipe(shared.data(), shared.size());
    return status;
}

void change_password_device(UsbDevice& dev, std::string_view old_password,
                            std::string_view new_password, const Login& login, const Log& log,
                            bool trace)
{
    if (new_password.empty() || new_password.size() > kMaxPasswordBytes)
        throw UnlockError("New password must be 1 to 16 bytes. Passphrase mode (10-64 "
                          "characters) is not supported yet");
    if (!dev.in_hid_mode())
        throw IronKeyError("Device is not in HID mode");

    HidrawDevice hid(dev.hidraw.front());
    TracingHidDevice traced(hid, log);
    Channel ch(trace ? static_cast<HidDevice&>(traced) : hid);
    change_password(ch, old_password, new_password, login.account);
    log("Password changed.");
}

DriveStatus read_drive_status(const UsbDevice& dev)
{
    DriveStatus status;
    on_read_channel(dev, [&](Channel& ch) {
        AreaParams p = read_area_params(ch);
        status.setup = p.has_user_account() ? PasswordSetup::AdminAndUser : PasswordSetup::Single;
        status.admin_failed_attempts = p.admin_failed_attempts;
    });
    return status;
}

DeviceSummary read_device_summary(const UsbDevice& dev)
{
    DeviceSummary s;
    s.name = describe(dev);
    s.unlocked = dev.is_unlocked();
    on_read_channel(dev, [&](Channel& ch) {
        AreaParams p = read_area_params(ch);
        s.setup = p.has_user_account() ? PasswordSetup::AdminAndUser : PasswordSetup::Single;
        s.admin_failed_attempts = p.admin_failed_attempts;
        s.status_read = true;
        // The owner-set name is a nice-to-have for telling drives apart; a
        // failure to read it (e.g. the profile session over SCSI) must not lose
        // the status we already have, so swallow it separately.
        try {
            s.owner_name = read_profile(ch).name;
        } catch (const std::exception&) {
        }
    });
    return s;
}

std::string Login::password_name() const
{
    if (account == Account::User)
        return "user password";
    return setup == PasswordSetup::Single ? "password" : "admin password";
}

std::string Login::lockout_consequence() const
{
    std::string n = std::to_string(kDefaultMaxFailedAttempts);
    if (account == Account::User)
        return "User login locks after " + n +
               " failed attempts in a row; the admin can reset it";
    return "The drive erases itself after " + n + " failed attempts in a row";
}

std::optional<int> Login::failed_attempts(const DriveStatus& status) const
{
    // Only the index 0 counter's location is known (PROTOCOL.md).
    return account == Account::Admin ? status.admin_failed_attempts : std::nullopt;
}

std::string Login::failed_warning(int failed) const
{
    if (failed <= 0)
        return {};
    int left = std::max(kDefaultMaxFailedAttempts - failed, 0);
    std::string who = setup == PasswordSetup::Single ? "" : std::string(to_string(account)) + " ";
    return std::to_string(failed) + " failed " + who + (failed == 1 ? "login" : "logins") +
           " since the last successful one; " + std::to_string(left) + " left before " +
           (account == Account::User ? "user login locks" : "the drive erases itself") +
           " (default limit " + std::to_string(kDefaultMaxFailedAttempts) + ")";
}

Login choose_login(const DriveStatus& status, std::optional<Account> requested)
{
    if (requested == Account::User && status.setup == PasswordSetup::Single)
        throw IronKeyError("This drive has only one password; it has no separate user account. "
                           "Unlock without --user");
    Login login;
    login.setup = status.setup;
    if (requested)
        login.account = *requested;
    else
        login.account = status.setup == PasswordSetup::AdminAndUser ? Account::User
                                                                    : Account::Admin;
    return login;
}

const char* const kUntrustedHostWarning =
    "Security: this drive's login uses RSA-512, which is weak. On a computer you\n"
    "do not trust, a program recording USB traffic could later recover the\n"
    "password you set here. Only do this on a machine you trust.";

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
