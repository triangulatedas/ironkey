// SPDX-License-Identifier: GPL-2.0-only
// ironkey-unlock: command-line front end.
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <thread>
#include <optional>
#include <string>
#include <string_view>

#include "linux/device_ops.h"
#include "linux/hidraw_device.h"
#include "linux/scsi_transport.h"
#include "linux/usb_topology.h"
#include "linux/volumes.h"
#include "proto/session.h"
#include "proto/tracing_device.h"

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

// Progress and trace output. Flushed immediately so it stays in order with
// errors printed to stderr when both go to the same file.
void print_log(const std::string& msg)
{
    std::printf("%s\n", msg.c_str());
    std::fflush(stdout);
}

// Password string that is wiped when it goes out of scope.
struct Password {
    std::string value;
    Password() { value.reserve(256); }  // avoid reallocations leaving copies behind
    ~Password() { secure_wipe(value.data(), value.capacity()); }
};

// Reads a line from the terminal with echo turned off.
bool read_password_tty(const std::string& prompt, Password& pw)
{
    int fd = ::open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return false;
    termios old{};
    bool have_termios = ::tcgetattr(fd, &old) == 0;
    if (have_termios) {
        termios quiet = old;
        quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        quiet.c_lflag |= ECHONL;
        ::tcsetattr(fd, TCSAFLUSH, &quiet);
    }
    (void)!::write(fd, prompt.data(), prompt.size());

    char c;
    while (::read(fd, &c, 1) == 1 && c != '\n')
        pw.value.push_back(c);
    secure_wipe(&c, 1);

    if (have_termios)
        ::tcsetattr(fd, TCSAFLUSH, &old);
    ::close(fd);
    return true;
}

bool read_password_stdin(Password& pw)
{
    int c;
    while ((c = std::getchar()) != EOF && c != '\n')
        pw.value.push_back(static_cast<char>(c));
    return !pw.value.empty();
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
            std::fprintf(stderr, "  %s\n", describe(d).c_str());
        return false;
    }
    out = std::move(keys.front());
    return true;
}

int cmd_hid_mode(const std::string& port)
{
    UsbDevice dev;
    if (!select_device(port, dev))
        return 1;
    ensure_hid_mode(dev, print_log);
    std::printf("%s is in HID mode (%s)\n", describe(dev).c_str(), dev.hidraw.front().c_str());
    return 0;
}

// Locates the setcap'd SCSI helper: $IRONKEY_SCSI_HELPER, else next to this
// executable, else on PATH.
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

// True if the replug-free SCSI path can be used right now: the drive is in
// normal storage mode and a harmless FF A0 read actually succeeds through the
// helper (so the helper exists, is setcap'd, and can reach the device).
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

// Diagnostic: read GetAreaParams (FF A0) over the SCSI/SG_IO path (via the
// helper) to validate the transport. The drive must be in normal storage mode.
int cmd_scsi_probe(const std::string& port)
{
    UsbDevice dev;
    if (!select_device(port, dev))
        return 1;
    if (dev.disks.empty()) {
        std::fprintf(stderr, "No storage node. The drive must be in normal (not HID) mode; "
                             "replug it if it is stuck in HID mode.\n");
        return 1;
    }
    std::string node = dev.disks.front().node;
    std::printf("SCSI probe on %s via %s\n", node.c_str(), find_scsi_helper().c_str());
    ScsiTransport st(find_scsi_helper(), node);
    Channel ch(st);
    ch.drain();
    AreaParams p = read_area_params(ch);
    std::printf("FF A0 OK: data_sectors=%u (%.1f GiB) unlocked=%d admin_failed=%u flags=0x%02x\n",
                p.data_sectors, p.data_sectors * 512.0 / (1u << 30), p.unlocked,
                p.admin_failed_attempts, p.account_flags);
    return 0;
}

// Diagnostic: unlock over SCSI (password on stdin) and watch whether the data
// LUN re-presents in place (the whole point of the SCSI path).
int cmd_scsi_unlock(const std::string& port)
{
    UsbDevice dev;
    if (!select_device(port, dev))
        return 1;
    if (dev.disks.empty()) {
        std::fprintf(stderr, "Drive must be in normal storage mode.\n");
        return 1;
    }
    Password pw;
    if (!read_password_stdin(pw) || pw.value.empty()) {
        std::fprintf(stderr, "No password on stdin.\n");
        return 1;
    }
    std::string node = dev.disks.front().node;
    {
        ScsiTransport st(find_scsi_helper(), node);
        Channel ch(st);
        ch.drain();
        AreaParams before = read_area_params(ch);
        Account acct = before.has_user_account() ? Account::User : Account::Admin;
        try {
            unlock(ch, pw.value, acct);
        } catch (const WrongPasswordError&) {
            std::fprintf(stderr, "Wrong password (SCSI).\n");
            return 3;
        }
        // Mode flag: in storage mode this is what flips the data LUN from
        // "medium not present" to ready (the capture reads full READ CAPACITY
        // right after it).
        SystemRandom rng;
        RsaKey host = RsaKey::generate512();
        Key128 shared = rsa_handshake(ch, rng, host);
        Bytes pvc = encrypt_pvc_key(kPvc0Key, shared, rng.bytes(32));
        ch.command(0x8B, 0x00, Direction::None, {}, 0, {0x00, 0x03});
        ch.command(0x8F, 0x00, Direction::Write, pvc, 0, {0x02});
        ch.command(0x26, 0x00, Direction::Write, Bytes{0x00, 0x02, 0x00}, 3, {0x00, 0x53});
        secure_wipe(shared.data(), shared.size());
        AreaParams after = read_area_params(ch);
        std::printf("FF A0 after login+modeflag: unlocked=%d (device-side)\n", after.unlocked);
    }
    // Did the kernel pick up the new capacity in place?
    for (int i = 0; i < 20; ++i) {
        refresh(dev);
        for (const auto& d : dev.disks)
            if (d.size_bytes > 0) {
                std::printf("Data disk present in place: %s (%s) — no replug!\n", d.node.c_str(),
                            human_size(d.size_bytes).c_str());
                return 0;
            }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    std::printf("Device reports unlocked but the kernel capacity is still 0 "
                "(%s). A rescan may be needed.\n",
                node.c_str());
    return 0;
}

// Diagnostic: RSA key exchange only. Sends no password and changes nothing.
int cmd_handshake(const std::string& port)
{
    UsbDevice dev;
    if (!select_device(port, dev))
        return 1;
    ensure_hid_mode(dev, print_log);

    HidrawDevice hid(dev.hidraw.front());
    Channel ch(hid);
    SystemRandom rng;
    RsaKey host = RsaKey::generate512();
    Key128 shared = rsa_handshake(ch, rng, host);
    secure_wipe(shared.data(), shared.size());
    std::puts("Handshake OK: session key established (FF 81..88).");
    return 0;
}

// Diagnostic: sends the read-only query commands GetConfig (0x21) and
// GetAreaParams (0xA0), before and after a key exchange, and dumps every
// exchange. Sends no password.
int cmd_dump(const std::string& port, bool with_session)
{
    UsbDevice dev;
    if (!select_device(port, dev))
        return 1;
    ensure_hid_mode(dev, print_log);
    std::printf("%s, %s\n", describe(dev).c_str(), dev.hidraw.front().c_str());

    HidrawDevice hid(dev.hidraw.front());
    TracingHidDevice traced(hid, [](const std::string& s) {
        std::fputs(s.c_str(), stdout);
        std::fflush(stdout);
    });
    Channel ch(traced);
    ch.drain();

    auto query = [&ch](uint8_t sub, const char* name) {
        std::printf("\n=== %s (FF %02X)\n", name, sub);
        try {
            ch.command(sub, 0x00, Direction::Read, {}, 0x200);
        } catch (const std::exception& e) {
            std::printf("    failed: %s\n", e.what());
            ch.drain();
        }
    };

    std::puts("\n##### Without a session");
    query(0x21, "GetConfig");
    query(0xA0, "GetAreaParams");

    if (!with_session)
        return 0;

    std::puts("\n##### After key exchange");
    SystemRandom rng;
    RsaKey host = RsaKey::generate512();
    Key128 shared = rsa_handshake(ch, rng, host);
    secure_wipe(shared.data(), shared.size());
    query(0x21, "GetConfig");
    query(0xA0, "GetAreaParams");
    return 0;
}

int cmd_unlock(const std::string& port, std::optional<Account> requested, bool password_stdin,
               bool trace)
{
    UsbDevice dev;
    if (!select_device(port, dev))
        return 1;
    if (dev.is_unlocked()) {
        std::printf("%s is already unlocked.\n", describe(dev).c_str());
        return 0;
    }
    check_access(dev);

    // Switch first so the password setup and failed-attempt count can be read
    // before the prompt. A locked drive in HID mode is harmless; replugging
    // returns it to normal.
    ensure_hid_mode(dev, print_log);
    DriveStatus status = read_drive_status(dev);
    Login login = choose_login(status, requested);
    if (status.setup == PasswordSetup::AdminAndUser)
        print_log(std::string("Drive has admin and user passwords; logging in with the ") +
                  login.password_name() +
                  (requested ? "" : login.account == Account::User ? " (use --admin for the admin)"
                                                                   : ""));
    if (auto failed = login.failed_attempts(status); failed && *failed > 0)
        std::fprintf(stderr, "Warning: %s.\n", login.failed_warning(*failed).c_str());

    Password pw;
    std::string name = login.password_name();
    name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    bool ok = password_stdin ? read_password_stdin(pw)
                             : read_password_tty(name + " for " + describe(dev) + ": ", pw);
    if (!ok || pw.value.empty()) {
        std::fprintf(stderr, "No password given.\n");
        return 1;
    }
    if (pw.value.size() > kMaxPassphraseBytes) {
        std::fprintf(stderr, "Password longer than 64 characters; nothing was sent.\n");
        return 1;
    }

    try {
        unlock_device(dev, pw.value, login, print_log, trace);
    } catch (const NotUnlockedError& e) {
        std::fprintf(stderr, "%s.\n", e.what());
        return 3;
    }

    for (const auto& d : dev.disks)
        if (d.size_bytes > 0)
            std::printf("Unlocked: %s (%s)\n", d.node.c_str(), human_size(d.size_bytes).c_str());
    return 0;
}

int cmd_change_password(const std::string& port, std::optional<Account> requested, bool trace)
{
    UsbDevice dev;
    if (!select_device(port, dev))
        return 1;
    check_access(dev);
    ensure_hid_mode(dev, print_log);
    Login login = choose_login(read_drive_status(dev), requested);

    std::fprintf(stderr, "%s\n\n", kUntrustedHostWarning);

    Password old_pw, new_pw, confirm;
    std::string name = login.password_name();
    if (!read_password_tty("Current " + name + ": ", old_pw) || old_pw.value.empty()) {
        std::fprintf(stderr, "No password given.\n");
        return 1;
    }
    read_password_tty("New " + name + ": ", new_pw);
    read_password_tty("Confirm new " + name + ": ", confirm);
    if (new_pw.value != confirm.value) {
        std::fprintf(stderr, "New passwords do not match; nothing was changed.\n");
        return 1;
    }
    if (new_pw.value.empty() || new_pw.value.size() > kMaxPasswordBytes) {
        std::fprintf(stderr, "New password must be 1 to 16 bytes (passphrase mode is not "
                             "supported yet); nothing was changed.\n");
        return 1;
    }

    try {
        change_password_device(dev, old_pw.value, new_pw.value, login, print_log, trace);
    } catch (const WrongPasswordError& e) {
        std::fprintf(stderr, "%s.\n", e.what());
        return 3;
    }
    std::printf("Changed the %s on %s.\n", name.c_str(), describe(dev).c_str());
    return 0;
}



// Diagnostic: probe the FF A2 area re-key token format WITHOUT committing, to
// test the reset hypothesis safely. Sends no FF 89, so nothing should persist.
int cmd_reset_probe(const std::string& port)
{
    UsbDevice dev;
    if (!select_device(port, dev))
        return 1;
    if (dev.is_unlocked()) {
        std::fprintf(stderr, "Lock the drive first (it is unlocked).\n");
        return 1;
    }
    check_access(dev);
    ensure_hid_mode(dev, print_log);

    std::puts("PROBE: sending FF A2 area re-key with a random token, NO commit (FF 89).");
    uint16_t status = rekey_area_probe(dev, print_log, /*commit=*/false, /*trace=*/true);
    if (status == 0)
        std::puts("\nFF A2 ACCEPTED (status 0): the token format looks right. "
                  "Nothing was committed.");
    else
        std::printf("\nFF A2 rejected (status 0x%04X): the token format is wrong.\n", status);
    return status == 0 ? 0 : 3;
}

// Diagnostic: replay captured FF 62 "write protected page" commands (the
// vendor Format step) read as hex lines on stdin: "<cdb_hex> <payload_hex>".
// Activates the data partition after a reset. DESTRUCTIVE to the data area.
int cmd_format_replay(const std::string& port)
{
    auto unhex = [](const std::string& s) {
        Bytes b;
        for (size_t i = 0; i + 1 < s.size(); i += 2)
            b.push_back(static_cast<uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
        return b;
    };
    std::vector<std::pair<uint8_t, Bytes>> pages;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        auto sp = line.find(' ');
        if (sp == std::string::npos)
            continue;
        Bytes cdb = unhex(line.substr(0, sp));
        Bytes payload = unhex(line.substr(sp + 1));
        if (cdb.size() < 4 || payload.size() != 512)
            continue;
        pages.emplace_back(cdb[3], std::move(payload));
    }
    if (pages.empty()) {
        std::fprintf(stderr, "No FF 62 page writes on stdin.\n");
        return 1;
    }
    std::printf("Replaying %zu protected-page writes.\n", pages.size());

    UsbDevice dev;
    if (!select_device(port, dev))
        return 1;
    check_access(dev);
    ensure_hid_mode(dev, print_log);
    HidrawDevice hid(dev.hidraw.front());
    Channel ch(hid);
    write_protected_pages(ch, pages);
    std::puts("Protected pages written. Re-reading area size.");

    refresh(dev);
    ensure_hid_mode(dev, print_log);
    DriveStatus st = read_drive_status(dev);
    std::printf("admin_failed=%s\n",
                st.admin_failed_attempts ? std::to_string(*st.admin_failed_attempts).c_str() : "?");
    return 0;
}

int cmd_lock(const std::string& port)
{
    // Only drives that are unlocked are candidates, so "the only IronKey"
    // means the only unlocked one.
    std::vector<UsbDevice> unlocked;
    for (auto& d : find_kingston_devices())
        if (d.is_ironkey() && d.is_unlocked() && (port.empty() || d.port == port))
            unlocked.push_back(std::move(d));
    if (unlocked.empty()) {
        std::printf(port.empty() ? "No unlocked IronKey found.\n"
                                 : "No unlocked IronKey on port %s.\n",
                    port.c_str());
        return 0;
    }
    if (unlocked.size() > 1) {
        std::fprintf(stderr, "Several IronKeys are unlocked; choose one with --device PORT:\n");
        for (const auto& d : unlocked)
            std::fprintf(stderr, "  %s\n", describe(d).c_str());
        return 1;
    }
    UsbDevice dev = std::move(unlocked.front());
    std::string name = describe(dev);
    lock_device(dev, print_log);
    std::printf("Locked %s. It is safe to unplug.\n", name.c_str());
    return 0;
}

// Creates a filesystem on an unlocked drive's data disk (via udisks, no root).
int cmd_format(const std::string& port, const std::string& fstype, const std::string& label)
{
    if (fstype != "vfat" && fstype != "exfat" && fstype != "ext4") {
        std::fprintf(stderr, "Unknown --fs '%s'. Use vfat, exfat or ext4.\n", fstype.c_str());
        return 2;
    }
    std::vector<UsbDevice> unlocked;
    for (auto& d : find_kingston_devices())
        if (d.is_ironkey() && d.is_unlocked() && (port.empty() || d.port == port))
            unlocked.push_back(std::move(d));
    if (unlocked.empty()) {
        std::fprintf(stderr, "No unlocked IronKey found. Unlock the drive first.\n");
        return 1;
    }
    if (unlocked.size() > 1) {
        std::fprintf(stderr, "Several IronKeys are unlocked; choose one with --device PORT.\n");
        return 1;
    }
    UsbDevice dev = std::move(unlocked.front());
    std::printf("Formatting the data disk of %s as %s (label '%s'). This erases it.\n",
                describe(dev).c_str(), fstype.c_str(), label.c_str());
    std::string node = format_data_disk(dev, fstype, label);
    std::printf("Done. %s is now a %s filesystem; eject with `ironkey-unlock lock`.\n",
                node.c_str(), fstype.c_str());
    return 0;
}

// Diagnostic: reset with a random password, then unlock with the SAME string,
// all programmatically. Removes human typing error from the test loop. The
// random password is shown so the drive stays usable afterwards.
int cmd_reset_selftest(const std::string& port)
{
    UsbDevice dev;
    if (!select_device(port, dev))
        return 1;
    check_access(dev);
    ensure_hid_mode(dev, print_log);

    SystemRandom rng;
    static const char charset[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    std::array<uint8_t, 12> raw{};
    rng.fill(raw);
    std::string pw;
    for (uint8_t b : raw)
        pw += charset[b % (sizeof charset - 1)];

    std::printf("Self-test: resetting with generated password: %s\n", pw.c_str());
    reset_device_full(dev, pw, Profile{}, print_log, /*trace=*/false);

    // The drive applies its new area layout on a power cycle, so the data disk
    // only presents after a replug. We cannot replug from here, so the self-test
    // stops at "password set" and hands the verification steps to the user.
    std::printf("\nReset done. Password is: %s\n"
                "Verify it end to end:\n"
                "  1. Unplug and replug the drive.\n"
                "  2. echo -n '%s' | ironkey-unlock unlock --password-stdin\n"
                "     The data disk should appear at full size.\n",
                pw.c_str(), pw.c_str());
    return 0;
}

// Reads the owner profile (name/company/description/hints) without unlocking.
int cmd_info(const std::string& port)
{
    UsbDevice dev;
    if (!select_device(port, dev))
        return 1;
    check_access(dev);
    ensure_hid_mode(dev, print_log);
    Profile p = read_profile_device(dev, print_log);
    if (p.empty()) {
        std::printf("No owner info set on %s.\n", describe(dev).c_str());
        return 0;
    }
    auto line = [](const char* label, const std::string& v) {
        if (!v.empty())
            std::printf("  %-12s %s\n", label, v.c_str());
    };
    std::printf("Owner info on %s:\n", describe(dev).c_str());
    line("Name:", p.name);
    line("Company:", p.company);
    line("Description:", p.description);
    line("Admin hint:", p.admin_hint);
    line("User hint:", p.user_hint);
    std::printf("(Readable without the password; stored on the drive.)\n");
    return 0;
}

// Experimental: send FF 01 (NTU_Detach_Attach) over SCSI to make the drive
// re-enumerate itself — an automatic "soft replug" after a reset. The three
// flags (Detach, Attach, Reboot) come from $IRONKEY_DAR (default "1,1,0") while
// we find the combination that works.
int cmd_scsi_reenum(const std::string& port)
{
    UsbDevice dev;
    if (!select_device(port, dev))
        return 1;
    if (dev.disks.empty()) {
        std::fprintf(stderr, "Need the drive in normal storage mode.\n");
        return 1;
    }
    unsigned d = 1, a = 1, r = 0;
    if (const char* e = std::getenv("IRONKEY_DAR"))
        std::sscanf(e, "%u,%u,%u", &d, &a, &r);
    auto f16 = [](Bytes& p, unsigned v) {
        p.push_back(v & 0xFF);
        p.push_back((v >> 8) & 0xFF);
        p.push_back(v & 0xFF);
        p.push_back(v & 0xFF);
    };
    Bytes payload;
    f16(payload, d);
    f16(payload, a);
    f16(payload, r);
    std::string node = dev.disks.front().node;
    std::string before = dev.usbfs_node;
    std::printf("FF 01 detach/attach on %s (Detach=%u Attach=%u Reboot=%u)\n", node.c_str(), d, a,
                r);
    try {
        ScsiTransport st(find_scsi_helper(), node);
        Channel ch(st);
        ch.command(0x01, 0x01, Direction::Write, payload, 12, {0, 0, 0, 0, 0, 0, 0, 0});
        std::printf("FF 01 accepted (status 0).\n");
    } catch (const std::exception& e) {
        std::printf("FF 01 returned: %s (device may be detaching, which is the point)\n", e.what());
    }
    std::printf("Watching for re-enumeration...\n");
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        UsbDevice now;
        if (select_device(port, now)) {
            if (now.usbfs_node != before || i == 29) {
                std::printf("Now: %s\n", describe(now).c_str());
                for (const auto& b : now.disks)
                    std::printf("  disk %s size=%s\n", b.node.c_str(),
                                human_size(b.size_bytes).c_str());
                return 0;
            }
        }
    }
    return 0;
}

// Factory reset / forgot-password recovery: wipes the drive (crypto-erase) and
// sets a brand-new password. No old password needed.
int cmd_reset(const std::string& port, bool password_stdin, std::optional<bool> prefer_scsi,
              const Profile& profile, bool with_user)
{
    UsbDevice dev;
    if (!select_device(port, dev))
        return 1;

    // Choose the transport: explicit --scsi/--hid wins; otherwise use SCSI
    // (no replug) when the helper is installed and working, else HID.
    bool use_scsi;
    if (prefer_scsi.has_value()) {
        use_scsi = *prefer_scsi;
        if (use_scsi && !scsi_usable(dev)) {
            std::fprintf(stderr, "SCSI reset is not available: the drive must be in normal "
                                 "storage mode and ironkey-scsi-helper must be installed with "
                                 "`setcap cap_sys_rawio+ep`.\n");
            return 1;
        }
    } else {
        use_scsi = scsi_usable(dev);
    }
    print_log(use_scsi ? "Using the SCSI path (no replug needed)."
                       : "Using the HID path (one replug needed after reset).");

    if (!use_scsi) {
        check_access(dev);
        ensure_hid_mode(dev, print_log);
    }

    std::fprintf(stderr,
                 "RESET erases ALL DATA on %s and sets a new password.\n"
                 "This cannot be undone. No current password is needed.\n\n%s\n\n",
                 describe(dev).c_str(), kUntrustedHostWarning);

    Password new_pw, user_pw;
    if (password_stdin) {
        // Scriptable: admin password on the first stdin line; with --admin-user,
        // the user password on the second line. No prompts.
        if (!read_password_stdin(new_pw)) {
            std::fprintf(stderr, "No password on stdin; nothing was changed.\n");
            return 1;
        }
        if (with_user && !read_password_stdin(user_pw)) {
            std::fprintf(stderr, "--admin-user needs a user password on the second stdin line; "
                                 "nothing was changed.\n");
            return 1;
        }
    } else {
        Password confirm_word, confirm;
        if (!read_password_tty("Type ERASE to continue: ", confirm_word) ||
            confirm_word.value != "ERASE") {
            std::fprintf(stderr, "Cancelled; nothing was changed.\n");
            return 1;
        }
        read_password_tty(with_user ? "New admin password: " : "New password: ", new_pw);
        read_password_tty("Confirm new password: ", confirm);
        if (new_pw.value != confirm.value) {
            std::fprintf(stderr, "Passwords do not match; nothing was changed.\n");
            return 1;
        }
        if (with_user) {
            Password user_confirm;
            read_password_tty("New user password: ", user_pw);
            read_password_tty("Confirm user password: ", user_confirm);
            if (user_pw.value != user_confirm.value) {
                std::fprintf(stderr, "User passwords do not match; nothing was changed.\n");
                return 1;
            }
        }
    }
    if (new_pw.value.empty() || new_pw.value.size() > kMaxPassphraseBytes) {
        std::fprintf(stderr, "New password must be 1 to 64 characters; nothing was changed.\n");
        return 1;
    }
    if (with_user && (user_pw.value.empty() || user_pw.value.size() > kMaxPassphraseBytes)) {
        std::fprintf(stderr, "User password must be 1 to 64 characters; nothing was changed.\n");
        return 1;
    }

    if (use_scsi) {
        {
            ScsiTransport st(find_scsi_helper(), dev.disks.front().node);
            Channel ch(st);
            reset_device(ch, new_pw.value, with_user ? user_pw.value : std::string());
            print_log(with_user ? "Reset complete: area wiped, admin and user passwords set."
                                : "Reset complete: area wiped and new password set.");
            if (!profile.empty()) {
                write_profile(ch, profile);
                print_log("Owner info written.");
            }
            // FF 01 (NTU_Detach_Attach, Reboot=1): a soft power-cycle that makes
            // the drive re-enumerate and apply the new area geometry — replaces
            // the physical replug. The device drops off the bus, so this command
            // does not return a status; the exception is expected.
            try {
                Bytes ff01 = {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1};
                ch.command(0x01, 0x01, Direction::Write, ff01, 12, {0, 0, 0, 0, 0, 0, 0, 0});
            } catch (const std::exception&) {
            }
        }
        print_log("Re-enumerating the drive (soft power-cycle)...");
        // Wait for the device to drop off the bus and come back, then let udev
        // settle (the node numbers and access ACLs change on re-enumeration).
        UsbDevice back;
        bool went_away = false;
        for (int i = 0; i < 50; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            bool present = select_device(port, back);
            if (!present)
                went_away = true;
            else if (went_away)
                break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::printf("\nReset complete over SCSI — no physical replug needed.\n"
                    "Next steps:\n"
                    "  1. ironkey-unlock unlock         (unlock with the new password)\n"
                    "  2. ironkey-unlock format --fs ext4   (create a filesystem)\n");
        return 0;
    }

    reset_device_full(dev, new_pw.value, profile, print_log, /*trace=*/false,
                      with_user ? user_pw.value : std::string());
    std::printf(
        "\nReset complete: the drive was wiped and the new password set.\n"
        "\nNext steps:\n"
        "  1. Unplug and replug the drive (a power cycle applies the new layout).\n"
        "  2. ironkey-unlock unlock         (unlock with the new password)\n"
        "  3. sudo mkfs.vfat -n IRONKEY /dev/sdX   (create a filesystem; use the\n"
        "     data disk shown by 'unlock', e.g. /dev/sda)\n");
    return 0;
}

void usage()
{
    std::puts(
        "Usage: ironkey-unlock [COMMAND] [--device PORT]\n"
        "\n"
        "Commands:\n"
        "  list                       Show connected Kingston devices (default)\n"
        "  unlock [--admin | --user] [--password-stdin] [--trace]\n"
        "                             Unlock a drive; prompts for the password.\n"
        "                             Drives with admin and user passwords log in with\n"
        "                             the user password unless --admin is given. Drives\n"
        "                             with a single password need neither option.\n"
        "                             --trace logs every HID exchange (secrets redacted)\n"
        "  change-password [--admin|--user]\n"
        "                             Change a password in place (prompts for current and\n"
        "                             new). Does not unlock the drive.\n"
        "  reset [--name N] [--company C] [--description D]\n"
        "        [--admin-hint H] [--user-hint H] [--admin-user] [--scsi|--hid]\n"
        "                             Forgot-password recovery: ERASE the drive and set a\n"
        "                             new password (no old password needed). Optional owner\n"
        "                             info is stored on the drive. --admin-user provisions a\n"
        "                             separate user account too (admin password first, then\n"
        "                             user; with --password-stdin, one per line). Uses the\n"
        "                             replug-free SCSI path automatically when the helper is\n"
        "                             installed, else HID (replug after). --scsi/--hid force it.\n"
        "  info                       Show owner info (name/company/hint) without unlocking\n"
        "  format [--fs vfat|exfat|ext4] [--label NAME]\n"
        "                             Create a filesystem on an unlocked drive's data disk\n"
        "                             (via udisks, no root). Default vfat, label IRONKEY.\n"
        "  lock                       Unmount and power off an unlocked drive, which\n"
        "                             locks it (same as Eject in a file manager)\n"
        "  hid-mode                   Switch a locked drive to HID mode\n"
        "  handshake                  Diagnostic: key exchange only, sends no password\n"
        "  dump [--no-session]        Diagnostic: dump the drive's config/status queries,\n"
        "                             before and after a key exchange. Sends no password.\n"
        "                             --no-session skips the key exchange\n"
        "\n"
        "--device PORT selects a drive by USB port (see 'list') when several are connected.");
}

}  // namespace

int main(int argc, char** argv)
{
    std::string_view cmd = argc > 1 ? argv[1] : "list";
    std::string port;
    bool password_stdin = false;
    bool trace = false;
    std::optional<Account> account;  // chosen from the drive's setup unless given
    bool with_session = true;
    Profile profile;             // owner info for `reset`
    std::string fstype = "vfat"; // for `format`
    std::string label = "IRONKEY";
    std::optional<bool> prefer_scsi; // for `reset`: nullopt=auto
    bool with_user = false;          // for `reset`: also provision a user account
    for (int i = 2; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--device" && i + 1 < argc) {
            port = argv[++i];
        } else if (cmd == "format" && arg == "--fs" && i + 1 < argc) {
            fstype = argv[++i];
        } else if (cmd == "format" && arg == "--label" && i + 1 < argc) {
            label = argv[++i];
        } else if (cmd == "reset" && arg == "--scsi") {
            prefer_scsi = true;
        } else if (cmd == "reset" && arg == "--hid") {
            prefer_scsi = false;
        } else if (cmd == "reset" && arg == "--admin-user") {
            with_user = true;
        } else if (arg == "--password-stdin" && (cmd == "unlock" || cmd == "reset")) {
            password_stdin = true;
        } else if (cmd == "reset" && i + 1 < argc &&
                   (arg == "--name" || arg == "--company" || arg == "--description" ||
                    arg == "--admin-hint" || arg == "--user-hint")) {
            std::string_view val = argv[++i];
            if (arg == "--name")
                profile.name = val;
            else if (arg == "--company")
                profile.company = val;
            else if (arg == "--description")
                profile.description = val;
            else if (arg == "--admin-hint")
                profile.admin_hint = val;
            else
                profile.user_hint = val;
        } else if (arg == "--trace" && cmd == "unlock") {
            trace = true;
        } else if (arg == "--trace" &&
                   cmd == "change-password") {
            trace = true;
        } else if (arg == "--user" &&
                   (cmd == "unlock" || cmd == "change-password")) {
            account = Account::User;
        } else if (arg == "--admin" &&
                   (cmd == "unlock" || cmd == "change-password")) {
            account = Account::Admin;
        } else if (arg == "--no-session" && cmd == "dump") {
            with_session = false;
        } else {
            usage();
            return 2;
        }
    }

    try {
        if (cmd == "list")
            return cmd_list();
        if (cmd == "unlock")
            return cmd_unlock(port, account, password_stdin, trace);
        if (cmd == "dump")
            return cmd_dump(port, with_session);
        if (cmd == "change-password")
            return cmd_change_password(port, account, trace);
        if (cmd == "reset-probe")
            return cmd_reset_probe(port);
        if (cmd == "reset")
            return cmd_reset(port, password_stdin, prefer_scsi, profile, with_user);
        if (cmd == "reset-selftest")
            return cmd_reset_selftest(port);
        if (cmd == "info")
            return cmd_info(port);
        if (cmd == "format")
            return cmd_format(port, fstype, label);
        if (cmd == "scsi-probe")
            return cmd_scsi_probe(port);
        if (cmd == "scsi-unlock")
            return cmd_scsi_unlock(port);
        if (cmd == "scsi-reenum")
            return cmd_scsi_reenum(port);
        if (cmd == "format-replay")
            return cmd_format_replay(port);
        if (cmd == "lock")
            return cmd_lock(port);
        if (cmd == "hid-mode")
            return cmd_hid_mode(port);
        if (cmd == "handshake")
            return cmd_handshake(port);
        if (cmd == "-h" || cmd == "--help" || cmd == "help") {
            usage();
            return 0;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
    usage();
    return 2;
}
