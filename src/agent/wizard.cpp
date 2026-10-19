// SPDX-License-Identifier: GPL-2.0-only
#include "agent/wizard.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/busy.h"
#include "agent/notify.h"
#include "linux/device_ops.h"
#include "linux/usb_topology.h"
#include "linux/volumes.h"
#include "proto/bytes.h"
#include "proto/session.h"
#include "version.h"

extern char** environ;

namespace ironkey {

namespace {

// The collected answers, from whichever front-end gathered them.
struct SetupRequest {
    bool with_user = false;
    std::string admin_pw, admin_hint;
    std::string user_pw, user_hint;
    std::string name, company, description;
    std::string format = "none";  // exfat | vfat | ext4 | none
    std::string label = "IRONKEY";
};

void wipe(std::string& s) { secure_wipe(s.data(), s.capacity()); }

void wipe(SetupRequest& r)
{
    wipe(r.admin_pw);
    wipe(r.user_pw);
}

bool find_drive(const std::string& port, UsbDevice& out)
{
    for (auto& d : find_kingston_devices())
        if (d.is_ironkey() && d.port == port) {
            out = std::move(d);
            return true;
        }
    return false;
}

std::string short_desc(const UsbDevice& dev)
{
    std::string s = dev.product.empty() ? "IronKey" : dev.product;
    if (!dev.port.empty())
        s += " (port " + dev.port + ")";
    return s;
}

// ---- generic child-process capture -----------------------------------------

struct Captured {
    bool spawned = false;
    int exit_code = -1;
    std::string out;
};

[[gnu::noinline]] Captured spawn_capture(const std::string& path,
                                         std::initializer_list<std::string_view> args)
{
    std::vector<std::string> owned{path};
    for (std::string_view a : args)
        owned.emplace_back(a);
    std::vector<char*> argv;
    argv.reserve(owned.size() + 1);
    for (auto& s : owned)
        argv.push_back(s.data());
    argv.push_back(nullptr);

    int pipefd[2];
    if (::pipe2(pipefd, O_CLOEXEC) < 0)
        return {};

    posix_spawn_file_actions_t act;
    posix_spawn_file_actions_init(&act);
    posix_spawn_file_actions_adddup2(&act, pipefd[1], STDOUT_FILENO);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t empty;
    sigemptyset(&empty);
    posix_spawnattr_setsigmask(&attr, &empty);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);

    pid_t pid;
    int rc = posix_spawnp(&pid, path.c_str(), &act, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&act);
    posix_spawnattr_destroy(&attr);
    ::close(pipefd[1]);
    if (rc != 0) {
        ::close(pipefd[0]);
        return {};
    }

    Captured c;
    c.spawned = true;
    char buf[256];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR))
        if (n > 0)
            c.out.append(buf, static_cast<size_t>(n));
    secure_wipe(buf, sizeof buf);
    ::close(pipefd[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    c.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return c;
}

// ===================== GTK front-end (ironkey-gui) ==========================

// $IRONKEY_GUI, else next to this executable, else "ironkey-gui" on PATH.
std::string find_gui()
{
    if (const char* e = std::getenv("IRONKEY_GUI"))
        return e;
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = '\0';
        std::string p(buf);
        auto slash = p.rfind('/');
        if (slash != std::string::npos)
            return p.substr(0, slash + 1) + "ironkey-gui";
    }
    return "ironkey-gui";
}

enum class GuiStatus { Ok, Cancelled, Unavailable };

void parse_setup(const std::string& out, SetupRequest& req)
{
    size_t pos = 0;
    while (pos < out.size()) {
        size_t nl = out.find('\n', pos);
        std::string line = out.substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? out.size() : nl + 1;
        auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "mode")
            req.with_user = (v == "adminuser");
        else if (k == "admin_password")
            req.admin_pw = v;
        else if (k == "admin_hint")
            req.admin_hint = v;
        else if (k == "user_password")
            req.user_pw = v;
        else if (k == "user_hint")
            req.user_hint = v;
        else if (k == "name")
            req.name = v;
        else if (k == "company")
            req.company = v;
        else if (k == "description")
            req.description = v;
        else if (k == "format")
            req.format = v;
        else if (k == "label")
            req.label = v;
    }
}

GuiStatus collect_via_gui(const std::string& desc, SetupRequest& req)
{
    Captured c = spawn_capture(find_gui(), {"setup", "--label", desc});
    GuiStatus gs;
    if (!c.spawned || c.exit_code == 127)
        gs = GuiStatus::Unavailable;
    else if (c.exit_code == 0)
        gs = GuiStatus::Ok;
    else
        gs = GuiStatus::Cancelled;
    if (gs == GuiStatus::Ok)
        parse_setup(c.out, req);
    secure_wipe(c.out.data(), c.out.capacity());
    return gs;
}

// ===================== shared: perform the reset ============================

void perform_setup(const std::string& port, const std::string& desc, SetupRequest& req)
{
    // The reset re-enumerates the drive (and the format step switches it to HID
    // and unlocks it); keep the agent's own unlock prompt off it until done.
    BusyPorts::Guard busy(port);
    if (!busy.owns()) {
        notify("IronKey", "Another operation is already running on this drive.", true);
        return;
    }
    notify("IronKey", "Resetting " + desc + "… this takes a few seconds.");
    UsbDevice fresh;
    if (!find_drive(port, fresh)) {
        notify("IronKey", "The drive is no longer connected; nothing was changed.", true);
        return;
    }
    Profile profile;
    profile.name = req.name;
    profile.company = req.company;
    profile.description = req.description;
    profile.admin_hint = req.admin_hint;
    profile.user_hint = req.user_hint;
    try {
        reset_device_auto(fresh, req.admin_pw, req.with_user ? req.user_pw : std::string(), profile,
                          std::nullopt, [](const std::string&) {});
    } catch (const std::exception& e) {
        notify("IronKey reset failed", e.what(), true);
        return;
    }

    bool do_format =
        req.format == "exfat" || req.format == "vfat" || req.format == "ext4";
    if (!do_format) {
        notify("IronKey set up", "Reset complete. Unlock the drive from the tray menu to use it.");
        return;
    }
    try {
        notify("IronKey", "Unlocking to format…");
        UsbDevice fdev;
        if (!find_drive(port, fdev))
            throw IronKeyError("the drive is no longer connected");
        ensure_hid_mode(fdev, [](const std::string&) {});
        Login login = choose_login(read_drive_status(fdev), Account::Admin);
        unlock_device(fdev, req.admin_pw, login, [](const std::string&) {});
        std::string node = format_data_disk(fdev, req.format, req.label);
        // Drop the marker so the Windows vendor client won't nag about cloud backup.
        write_file_to_data_disk(node, "USBtoCloudInstallDeclined.txt", "");
        notify("IronKey ready", "Set up complete. Formatted " + node + " as " + req.format + ".");
    } catch (const std::exception& e) {
        notify("IronKey",
               std::string("The drive was reset, but formatting failed: ") + e.what() +
                   "\nYou can unlock and format it from the tray.",
               true);
    }
}

}  // namespace

void run_setup_wizard(std::string port, bool dry_run)
{
    UsbDevice dev;
    if (!find_drive(port, dev)) {
        notify("IronKey", "The drive is no longer connected.", true);
        return;
    }
    const std::string desc = short_desc(dev);

    SetupRequest req;
    GuiStatus gs = collect_via_gui(desc, req);
    if (gs == GuiStatus::Unavailable) {
        notify("IronKey", "The setup dialog needs the ironkey-gui package. Install it, or use "
                          "`ironkey-unlock reset`.",
               true);
        return;
    }
    if (gs == GuiStatus::Cancelled)
        return;

    if (dry_run) {
        notify("IronKey dry run",
               "Setup collected for " + desc +
                   (req.with_user ? " (admin + user)." : " (single password).") +
                   " Nothing was sent to the drive.");
        wipe(req);
        return;
    }

    perform_setup(port, desc, req);
    wipe(req);
}

// ---- unlocked-drive menu actions -------------------------------------------

void open_data_disk(std::string port)
{
    UsbDevice dev;
    if (!find_drive(port, dev) || !dev.is_unlocked()) {
        notify("IronKey", "The drive is not unlocked.", true);
        return;
    }
    // Find the mount point of one of the drive's data partitions.
    std::string mount;
    if (FILE* f = std::fopen("/proc/mounts", "re")) {
        char src[512], tgt[512];
        while (std::fscanf(f, "%511s %511s %*[^\n]\n", src, tgt) == 2) {
            std::string s(src);
            for (const auto& d : dev.disks)
                if (!d.node.empty() && s.rfind(d.node, 0) == 0) {  // src starts with disk node
                    mount = tgt;
                    break;
                }
            if (!mount.empty())
                break;
        }
        std::fclose(f);
    }
    if (mount.empty()) {
        notify("IronKey", "Could not find the drive's mounted folder.", true);
        return;
    }
    spawn_capture("xdg-open", {mount});
}

void run_change_password_op(std::string port)
{
    UsbDevice dev;
    if (!find_drive(port, dev)) {
        notify("IronKey", "The drive is no longer connected.", true);
        return;
    }
    const std::string desc = short_desc(dev);
    bool admin_user = read_device_summary(dev).setup == PasswordSetup::AdminAndUser;

    Captured c = admin_user
                     ? spawn_capture(find_gui(), {"changepw", "--label", desc, "--accounts",
                                                  "adminuser"})
                     : spawn_capture(find_gui(), {"changepw", "--label", desc});
    if (!c.spawned || c.exit_code == 127) {
        notify("IronKey", "The GTK dialog (ironkey-gui) is not available. Use "
                          "`ironkey-unlock change-password`.",
               true);
        return;
    }
    if (c.exit_code != 0)
        return;  // cancelled

    std::string old_pw, new_pw, acc = "user";
    size_t pos = 0;
    while (pos < c.out.size()) {
        size_t nl = c.out.find('\n', pos);
        std::string line = c.out.substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? c.out.size() : nl + 1;
        auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "old")
            old_pw = v;
        else if (k == "new")
            new_pw = v;
        else if (k == "account")
            acc = v;
    }
    secure_wipe(c.out.data(), c.out.capacity());
    Account account = Account::Admin;
    if (admin_user)
        account = (acc == "admin") ? Account::Admin : Account::User;

    BusyPorts::Guard busy(port);
    if (!busy.owns()) {
        notify("IronKey", "Another operation is already running on this drive.", true);
        wipe(old_pw);
        wipe(new_pw);
        return;
    }
    try {
        UsbDevice fresh;
        if (!find_drive(port, fresh))
            throw IronKeyError("the drive is no longer connected");
        ensure_hid_mode(fresh, [](const std::string&) {});
        Login login = choose_login(read_drive_status(fresh), account);
        change_password_device(fresh, old_pw, new_pw, login, [](const std::string&) {});
        notify("IronKey", "Password changed. Unlock the drive to use it.");
    } catch (const std::exception& e) {
        notify("IronKey", std::string("Could not change the password: ") + e.what(), true);
    }
    wipe(old_pw);
    wipe(new_pw);
}

void run_reset_user_password_op(std::string port)
{
    UsbDevice dev;
    if (!find_drive(port, dev)) {
        notify("IronKey", "The drive is no longer connected.", true);
        return;
    }
    if (read_device_summary(dev).setup != PasswordSetup::AdminAndUser) {
        notify("IronKey", "This drive has no separate user account to reset.", true);
        return;
    }
    const std::string desc = short_desc(dev);
    Captured c = spawn_capture(find_gui(), {"resetuserpw", "--label", desc});
    if (!c.spawned || c.exit_code == 127) {
        notify("IronKey", "The GTK dialog (ironkey-gui) is not available. Use "
                          "`ironkey-unlock reset-user-password`.",
               true);
        return;
    }
    if (c.exit_code != 0)
        return;  // cancelled

    std::string admin, new_user;
    size_t pos = 0;
    while (pos < c.out.size()) {
        size_t nl = c.out.find('\n', pos);
        std::string line = c.out.substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? c.out.size() : nl + 1;
        auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "admin")
            admin = v;
        else if (k == "new")
            new_user = v;
    }
    secure_wipe(c.out.data(), c.out.capacity());
    BusyPorts::Guard busy(port);
    if (!busy.owns()) {
        notify("IronKey", "Another operation is already running on this drive.", true);
        wipe(admin);
        wipe(new_user);
        return;
    }
    try {
        UsbDevice fresh;
        if (!find_drive(port, fresh))
            throw IronKeyError("the drive is no longer connected");
        ensure_hid_mode(fresh, [](const std::string&) {});
        reset_user_password_device(fresh, admin, new_user, [](const std::string&) {});
        notify("IronKey", "User password reset. The user can unlock with the new password.");
    } catch (const WrongPasswordError&) {
        notify("IronKey", "The admin password is wrong; nothing was changed.", true);
    } catch (const std::exception& e) {
        notify("IronKey", std::string("Could not reset the user password: ") + e.what(), true);
    }
    wipe(admin);
    wipe(new_user);
}

void run_format_op(std::string port)
{
    UsbDevice dev;
    if (!find_drive(port, dev) || !dev.is_unlocked()) {
        notify("IronKey", "Unlock the drive before formatting.", true);
        return;
    }
    const std::string desc = short_desc(dev);
    Captured c = spawn_capture(find_gui(), {"format", "--label", desc});
    if (!c.spawned || c.exit_code == 127) {
        notify("IronKey",
               "The GTK dialog (ironkey-gui) is not available. Use `ironkey-unlock format`.", true);
        return;
    }
    if (c.exit_code != 0)
        return;  // cancelled

    std::string fstype, label = "IRONKEY";
    size_t pos = 0;
    while (pos < c.out.size()) {
        size_t nl = c.out.find('\n', pos);
        std::string line = c.out.substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? c.out.size() : nl + 1;
        auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "format")
            fstype = v;
        else if (k == "label")
            label = v;
    }
    if (fstype != "exfat" && fstype != "vfat" && fstype != "ext4")
        return;
    BusyPorts::Guard busy(port);
    if (!busy.owns()) {
        notify("IronKey", "Another operation is already running on this drive.", true);
        return;
    }
    try {
        UsbDevice fresh;
        if (!find_drive(port, fresh) || !fresh.is_unlocked())
            throw IronKeyError("the drive is no longer unlocked");
        std::string node = format_data_disk(fresh, fstype, label);
        write_file_to_data_disk(node, "USBtoCloudInstallDeclined.txt", "");
        notify("IronKey ready", "Formatted " + node + " as " + fstype + ".");
    } catch (const std::exception& e) {
        notify("IronKey", std::string("Format failed: ") + e.what(), true);
    }
}

void show_about()
{
    Captured c = spawn_capture(find_gui(), {"about"});
    if (!c.spawned || c.exit_code == 127)
        notify("IronKey", "IronKey " IRONKEY_VERSION_FULL ", " IRONKEY_COMPANY
                          ". Licensed under the GNU GPL v2. Install ironkey-gui for the full "
                          "About dialog.");
}

}  // namespace ironkey
