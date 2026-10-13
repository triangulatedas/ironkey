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

// Spawns zenity with `args`, returns its stdout (newline-trimmed) on exit 0, or
// nullopt on cancel / error / zenity missing. No shell is involved and no user
// data is passed on the command line (typed values come back on stdout), so the
// fixed-string args are safe. noinline: it forks a process (inlining is
// pointless) and it sidesteps a GCC false-positive -Wfree-nonheap-object when
// the argument list is inlined into the caller.
[[gnu::noinline]] std::optional<std::string> run_zenity(std::initializer_list<std::string_view> args)
{
    std::vector<std::string> owned;
    owned.reserve(args.size());
    for (std::string_view a : args)
        owned.emplace_back(a);

    std::vector<char*> argv;
    argv.reserve(owned.size() + 2);
    argv.push_back(const_cast<char*>("zenity"));
    for (auto& s : owned)
        argv.push_back(s.data());
    argv.push_back(nullptr);

    int pipefd[2];
    if (::pipe2(pipefd, O_CLOEXEC) < 0)
        return std::nullopt;

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
    int rc = posix_spawnp(&pid, "zenity", &act, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&act);
    posix_spawnattr_destroy(&attr);
    ::close(pipefd[1]);
    if (rc != 0) {
        ::close(pipefd[0]);
        return std::nullopt;
    }

    std::string out;
    char buf[256];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR))
        if (n > 0)
            out.append(buf, static_cast<size_t>(n));
    secure_wipe(buf, sizeof buf);
    ::close(pipefd[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (!out.empty() && out.back() == '\n')
        out.pop_back();
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        secure_wipe(out.data(), out.capacity());
        return std::nullopt;
    }
    return out;
}

// A hidden-text (password) entry. nullopt = cancelled.
std::optional<std::string> ask_secret(const std::string& title, const std::string& text)
{
    return run_zenity({"--entry", "--hide-text", "--width=460", "--title", title, "--text", text});
}

// A visible entry with an optional prefilled value. nullopt = cancelled.
std::optional<std::string> ask_text(const std::string& title, const std::string& text,
                                    const std::string& prefill = "")
{
    if (prefill.empty())
        return run_zenity({"--entry", "--width=460", "--title", title, "--text", text});
    return run_zenity(
        {"--entry", "--width=460", "--title", title, "--text", text, "--entry-text", prefill});
}

void warn_box(const std::string& text)
{
    run_zenity({"--warning", "--icon=dialog-warning", "--width=420", "--text", text});
}

// A yes/no confirmation shown with a warning icon. true = the user confirmed.
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

std::string short_desc(const UsbDevice& dev)
{
    std::string s = dev.product.empty() ? "IronKey" : dev.product;
    if (!dev.port.empty())
        s += " (port " + dev.port + ")";
    return s;
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

// Collects a confirmed password plus an optional hint for one account. `who` is
// "" (single), "admin " or "user ". Returns false if the user cancelled.
bool collect_password(const std::string& who, std::string& out_pw, std::string& out_hint)
{
    for (;;) {
        auto pw = ask_secret("Set the " + who + "password",
                             "Choose a password of <b>at least 6 characters</b>.\n"
                             "10 or more, mixing letters, digits and symbols, is strongly "
                             "recommended.");
        if (!pw)
            return false;
        if (pw->size() < 6) {
            warn_box("The password must be at least 6 characters.");
            secure_wipe(pw->data(), pw->capacity());
            continue;
        }
        const char* strength = password_strength(*pw);
        auto confirm = ask_secret("Confirm password", std::string("Strength: <b>") + strength +
                                                           "</b>\nRe-enter the same password.");
        if (!confirm) {
            secure_wipe(pw->data(), pw->capacity());
            return false;
        }
        bool match = (*pw == *confirm);
        secure_wipe(confirm->data(), confirm->capacity());
        if (!match) {
            warn_box("The passwords did not match. Please try again.");
            secure_wipe(pw->data(), pw->capacity());
            continue;
        }
        if (pw->size() < 10 &&
            !confirm_warn("This password is short (" + std::to_string(pw->size()) +
                              " characters, strength <b>" + strength +
                              "</b>).\nA longer passphrase is much harder to guess.\n\nUse it "
                              "anyway?",
                          "Use it anyway", "Change it")) {
            secure_wipe(pw->data(), pw->capacity());
            continue;
        }
        auto hint = ask_text("Password hint (optional)",
                             "Shown on the drive <b>before unlocking</b>, on any computer.\n"
                             "A reminder only — do not put the password itself here.");
        out_pw = *pw;
        secure_wipe(pw->data(), pw->capacity());
        out_hint = hint ? *hint : std::string();
        return true;
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

    // 1. Confirm erase (warning icon, short formatted text).
    if (!confirm_warn("<b>Erase “" + desc +
                          "” and set it up?</b>\n\nThis permanently deletes everything on the "
                          "drive. You will choose a new password next; no current password is "
                          "needed.",
                      "Erase and set up", "Cancel"))
        return;

    // 2. Optional second (user) account — framed as a choice, not a count.
    auto mode = run_zenity(
        {"--list", "--radiolist", "--title", "User account", "--width=460", "--height=300",
         "--print-column", "2", "--hide-column", "2", "--text",
         "A separate <b>user</b> account is optional.\n\nIt has its own password. The <b>admin</b> "
         "can reset the user's password if it is forgotten; the user cannot change the admin's. "
         "Most people need just one password.",
         "--column", "", "--column", "key", "--column", "Set up", "TRUE", "single",
         "Just one password  (recommended)", "FALSE", "both",
         "Admin  +  a separate user account"});
    if (!mode)
        return;
    const bool with_user = (*mode == "both");

    // 3. Password(s), each with an optional hint.
    Profile profile;
    std::string admin, user;
    if (!collect_password(with_user ? "admin " : "", admin, profile.admin_hint))
        return;
    if (with_user && !collect_password("user ", user, profile.user_hint)) {
        secure_wipe(admin.data(), admin.capacity());
        return;
    }

    // 4. Owner info (optional): name, company, description. Cancel = skip.
    if (auto info = run_zenity({"--forms", "--title", "Owner info (optional)", "--width=480",
                               "--separator", std::string(1, '\x1f'), "--text",
                               "Stored on the drive and readable before unlock. Leave blank to "
                               "skip.",
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
            profile.name = f[0];
        if (f.size() > 1)
            profile.company = f[1];
        if (f.size() > 2)
            profile.description = f[2];
    }

    if (dry_run) {
        notify("IronKey dry run",
               "Setup collected for " + desc +
                   (with_user ? " (admin + user)." : " (single password).") +
                   " Nothing was sent to the drive.");
        secure_wipe(admin.data(), admin.capacity());
        secure_wipe(user.data(), user.capacity());
        return;
    }

    // 5. Reset. Re-find the drive first (nodes may have changed).
    notify("IronKey", "Resetting " + desc + "… this takes a few seconds.");
    UsbDevice fresh;
    if (!find_drive(port, fresh)) {
        secure_wipe(admin.data(), admin.capacity());
        secure_wipe(user.data(), user.capacity());
        notify("IronKey", "The drive is no longer connected; nothing was changed.", true);
        return;
    }
    try {
        reset_device_auto(fresh, admin, user, profile, std::nullopt, [](const std::string&) {});
    } catch (const std::exception& e) {
        secure_wipe(admin.data(), admin.capacity());
        secure_wipe(user.data(), user.capacity());
        notify("IronKey reset failed", e.what(), true);
        return;
    }

    // 6. Optional format. Needs the drive unlocked, so do it with the admin
    //    password we just set.
    auto fs = run_zenity(
        {"--list", "--radiolist", "--title", "Format the data disk?", "--width=480", "--height=300",
         "--print-column", "2", "--hide-column", "2", "--text",
         "Optionally create a filesystem now. Pick where you will use the drive:", "--column", "",
         "--column", "key", "--column", "Filesystem", "TRUE", "exfat",
         "exFAT — Windows, macOS and Linux", "FALSE", "vfat",
         "FAT32 — most compatible, but no files over 4 GB", "FALSE", "ext4",
         "ext4 — Linux only, owned by you"});
    if (fs) {
        auto label = ask_text("Volume label", "A name for the formatted disk.", "IRONKEY");
        std::string lbl = (label && !label->empty()) ? *label : "IRONKEY";
        try {
            notify("IronKey", "Unlocking to format… ");
            UsbDevice fdev;
            if (!find_drive(port, fdev))
                throw IronKeyError("the drive is no longer connected");
            ensure_hid_mode(fdev, [](const std::string&) {});
            Login login = choose_login(read_drive_status(fdev), Account::Admin);
            unlock_device(fdev, admin, login, [](const std::string&) {});
            std::string node = format_data_disk(fdev, *fs, lbl);
            notify("IronKey ready", "Set up complete. Formatted " + node + " as " + *fs + ".");
        } catch (const std::exception& e) {
            notify("IronKey",
                   std::string("The drive was reset, but formatting failed: ") + e.what() +
                       "\nYou can unlock and format it from the tray.",
                   true);
        }
    } else {
        notify("IronKey set up", "Reset complete. Unlock the drive from the tray menu to use it.");
    }

    secure_wipe(admin.data(), admin.capacity());
    secure_wipe(user.data(), user.capacity());
}

}  // namespace ironkey
