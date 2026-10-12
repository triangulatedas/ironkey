// SPDX-License-Identifier: GPL-2.0-only
// ironkey-unlock: command-line front end.
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

#include "linux/device_ops.h"
#include "linux/hidraw_device.h"
#include "linux/usb_topology.h"
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
    if (pw.value.size() > kMaxPasswordBytes) {
        std::fprintf(stderr, "Password longer than 16 bytes; nothing was sent. Passphrase mode "
                             "(10-64 characters) is not supported yet.\n");
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
    for (int i = 2; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--device" && i + 1 < argc) {
            port = argv[++i];
        } else if (arg == "--password-stdin" && cmd == "unlock") {
            password_stdin = true;
        } else if (arg == "--trace" && cmd == "unlock") {
            trace = true;
        } else if (arg == "--user" && cmd == "unlock") {
            account = Account::User;
        } else if (arg == "--admin" && cmd == "unlock") {
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
