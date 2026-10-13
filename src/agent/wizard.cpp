// SPDX-License-Identifier: GPL-2.0-only
#include "agent/wizard.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/notify.h"
#include "linux/device_ops.h"
#include "linux/usb_topology.h"
#include "linux/volumes.h"
#include "proto/bytes.h"
#include "proto/session.h"

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

// ===================== zenity fallback front-end ============================

[[gnu::noinline]] std::optional<std::string> run_zenity(std::initializer_list<std::string_view> args)
{
    Captured c = spawn_capture("zenity", args);
    if (!c.spawned || c.exit_code != 0) {
        secure_wipe(c.out.data(), c.out.capacity());
        return std::nullopt;
    }
    if (!c.out.empty() && c.out.back() == '\n')
        c.out.pop_back();
    return c.out;
}

std::optional<std::string> ask_secret(const std::string& title, const std::string& text)
{
    return run_zenity({"--entry", "--hide-text", "--width=460", "--title", title, "--text", text});
}

std::optional<std::string> ask_text(const std::string& title, const std::string& text)
{
    return run_zenity({"--entry", "--width=460", "--title", title, "--text", text});
}

void warn_box(const std::string& text)
{
    run_zenity({"--warning", "--icon=dialog-warning", "--width=420", "--text", text});
}

bool confirm_warn(const std::string& text, const std::string& ok, const std::string& cancel)
{
    return run_zenity({"--question", "--icon=dialog-warning", "--width=480", "--default-cancel",
                       "--text", text, "--ok-label", ok, "--cancel-label", cancel})
        .has_value();
}

const char* password_strength(const std::string& p)
{
    bool lo = false, up = false, di = false, ot = false;
    for (unsigned char c : p) {
        if (std::islower(c))
            lo = true;
        else if (std::isupper(c))
            up = true;
        else if (std::isdigit(c))
            di = true;
        else
            ot = true;
    }
    int classes = lo + up + di + ot;
    size_t n = p.size();
    if (n >= 12 && classes >= 3)
        return "Strong";
    if (n >= 10 && classes >= 2)
        return "Good";
    if (n >= 8)
        return "Fair";
    return "Weak";
}

bool collect_password_zenity(const std::string& who, std::string& out_pw, std::string& out_hint)
{
    for (;;) {
        auto pw = ask_secret("Set the " + who + "password",
                             "Choose a password of <b>at least 6 characters</b>. 10 or more is "
                             "strongly recommended.");
        if (!pw)
            return false;
        if (pw->size() < 6) {
            warn_box("The password must be at least 6 characters.");
            wipe(*pw);
            continue;
        }
        const char* s = password_strength(*pw);
        auto cf = ask_secret("Confirm password",
                             std::string("Strength: <b>") + s + "</b>\nRe-enter the same password.");
        if (!cf) {
            wipe(*pw);
            return false;
        }
        bool match = (*pw == *cf);
        wipe(*cf);
        if (!match) {
            warn_box("The passwords did not match. Please try again.");
            wipe(*pw);
            continue;
        }
        if (pw->size() < 10 &&
            !confirm_warn("Short password (" + std::to_string(pw->size()) + " chars, strength <b>" +
                              s + "</b>). Use it anyway?",
                          "Use it anyway", "Change it")) {
            wipe(*pw);
            continue;
        }
        auto hint = ask_text("Password hint (optional)",
                             "Shown before unlock, on any computer. Not the password itself.");
        out_pw = *pw;
        wipe(*pw);
        out_hint = hint ? *hint : std::string();
        return true;
    }
}

// Returns false if the user cancelled.
bool collect_via_zenity(const UsbDevice& dev, SetupRequest& req)
{
    const std::string desc = short_desc(dev);
    if (!confirm_warn("<b>Erase “" + desc +
                          "” and set it up?</b>\n\nThis permanently deletes everything on the "
                          "drive. You will choose a new password next; no current password is "
                          "needed.",
                      "Erase and set up", "Cancel"))
        return false;

    auto mode = run_zenity(
        {"--list", "--radiolist", "--title", "User account", "--width=460", "--height=300",
         "--print-column", "2", "--hide-column", "2", "--text",
         "A separate <b>user</b> account is optional. The admin can reset the user's password; the "
         "user cannot change the admin's. Most people need just one.",
         "--column", "", "--column", "key", "--column", "Set up", "TRUE", "single",
         "Just one password  (recommended)", "FALSE", "both", "Admin + a separate user account"});
    if (!mode)
        return false;
    req.with_user = (*mode == "both");

    if (!collect_password_zenity(req.with_user ? "admin " : "", req.admin_pw, req.admin_hint))
        return false;
    if (req.with_user && !collect_password_zenity("user ", req.user_pw, req.user_hint))
        return false;

    if (auto info = run_zenity({"--forms", "--title", "Owner info (optional)", "--width=480",
                               "--separator", std::string(1, '\x1f'), "--text",
                               "Stored on the drive, readable before unlock. Leave blank to skip.",
                               "--add-entry", "Name", "--add-entry", "Company", "--add-entry",
                               "Description"})) {
        std::vector<std::string> f;
        size_t start = 0;
        for (;;) {
            size_t sep = info->find('\x1f', start);
            f.push_back(info->substr(start, sep - start));
            if (sep == std::string::npos)
                break;
            start = sep + 1;
        }
        if (f.size() > 0)
            req.name = f[0];
        if (f.size() > 1)
            req.company = f[1];
        if (f.size() > 2)
            req.description = f[2];
    }

    auto fs = run_zenity(
        {"--list", "--radiolist", "--title", "Format the data disk?", "--width=480", "--height=300",
         "--print-column", "2", "--hide-column", "2", "--text",
         "Optionally create a filesystem now:", "--column", "", "--column", "key", "--column",
         "Filesystem", "TRUE", "exfat", "exFAT — Windows, macOS and Linux", "FALSE", "vfat",
         "FAT32 — most compatible, no files over 4 GB", "FALSE", "ext4", "ext4 — Linux only",
         "FALSE", "none", "Don't format now"});
    req.format = fs ? *fs : "none";
    if (req.format != "none" && req.format != "exfat" && req.format != "vfat" &&
        req.format != "ext4")
        req.format = "none";
    if (req.format != "none") {
        auto label = ask_text("Volume label", "A name for the formatted disk.");
        if (label && !label->empty())
            req.label = *label;
    }
    return true;
}

// ===================== shared: perform the reset ============================

void perform_setup(const std::string& port, const std::string& desc, SetupRequest& req)
{
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
    if (gs == GuiStatus::Cancelled)
        return;
    if (gs == GuiStatus::Unavailable) {
        // No ironkey-gui; fall back to the zenity dialog sequence.
        if (!collect_via_zenity(dev, req)) {
            wipe(req);
            return;
        }
    }

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
    try {
        UsbDevice fresh;
        if (!find_drive(port, fresh) || !fresh.is_unlocked())
            throw IronKeyError("the drive is no longer unlocked");
        std::string node = format_data_disk(fresh, fstype, label);
        notify("IronKey ready", "Formatted " + node + " as " + fstype + ".");
    } catch (const std::exception& e) {
        notify("IronKey", std::string("Format failed: ") + e.what(), true);
    }
}

}  // namespace ironkey
