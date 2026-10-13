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

#include "agent/notify.h"
#include "linux/device_ops.h"
#include "linux/usb_topology.h"
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
// the std::vector<std::string> arg list is inlined into the caller.
[[gnu::noinline]] std::optional<std::string> run_zenity(std::initializer_list<std::string_view> args)
{
    // Own null-terminated copies here (string_view is not terminated). Keeping
    // this vector inside this noinline function means the caller builds only a
    // trivially-destructible string_view array at the call site.
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

std::optional<std::string> ask_password(const std::string& title, const std::string& text)
{
    return run_zenity({"--entry", "--hide-text", "--width=440", "--title", title, "--text", text});
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

void wipe(std::optional<std::string>& s)
{
    if (s)
        secure_wipe(s->data(), s->capacity());
}

const char* const kHostWarning =
    "Set a password only on a computer you trust: this drive's unlock uses weak "
    "RSA that a program recording USB traffic could break.";

}  // namespace

void run_setup_wizard(std::string port, bool dry_run)
{
    UsbDevice dev;
    if (!find_drive(port, dev)) {
        notify("IronKey", "The drive is no longer connected.", true);
        return;
    }
    const std::string desc = describe(dev);

    // 1. Confirm erase.
    if (!run_zenity({"--question", "--title", "Reset IronKey", "--width=480", "--default-cancel",
                     "--ok-label", "Erase and set up", "--cancel-label", "Cancel", "--text",
                     "This ERASES ALL DATA on " + desc +
                         " and sets a new password.\nThis cannot be undone, and no current "
                         "password is needed.\n\nContinue?"}))
        return;

    // 2. One password or admin + user.
    auto mode = run_zenity({"--list", "--radiolist", "--title", "Accounts", "--width=400",
                            "--height=260", "--print-column", "2", "--text",
                            "Set up how many passwords on this drive?", "--column", "", "--column",
                            "Mode", "TRUE", "Single password", "FALSE", "Admin + user"});
    if (!mode)
        return;
    const bool with_user = (*mode == "Admin + user");

    // 3. Password(s), each entered twice. Separate dialogs avoid any form-field
    //    separator colliding with the password.
    auto admin = ask_password(with_user ? "New admin password" : "New password", kHostWarning);
    if (!admin)
        return;
    auto admin2 = ask_password("Confirm password", "Re-enter the same password.");
    if (!admin2) {
        wipe(admin);
        return;
    }
    bool ok = (*admin == *admin2) && !admin->empty() && admin->size() <= kMaxPassphraseBytes;
    wipe(admin2);
    if (!ok) {
        wipe(admin);
        notify("IronKey", "Passwords did not match or were not 1–64 characters; nothing was "
                          "changed.",
               true);
        return;
    }

    std::optional<std::string> user;
    if (with_user) {
        user = ask_password("New user password", kHostWarning);
        if (!user) {
            wipe(admin);
            return;
        }
        auto user2 = ask_password("Confirm user password", "Re-enter the user password.");
        bool uok = user2 && (*user == *user2) && !user->empty() && user->size() <= kMaxPassphraseBytes;
        wipe(user2);
        if (!uok) {
            wipe(admin);
            wipe(user);
            notify("IronKey", "User passwords did not match or were not 1–64 characters; nothing "
                              "was changed.",
                   true);
            return;
        }
    }

    // 4. Owner info (optional). Cancel just skips it.
    Profile profile;
    if (auto info = run_zenity({"--forms", "--title", "Owner info (optional)", "--width=440",
                               "--separator", std::string(1, '\x1f'), "--text",
                               "Stored on the drive and readable before unlock. Leave blank to "
                               "skip.",
                               "--add-entry", "Name", "--add-entry", "Admin password hint"})) {
        auto sep = info->find('\x1f');
        if (sep != std::string::npos) {
            profile.name = info->substr(0, sep);
            profile.admin_hint = info->substr(sep + 1);
        } else {
            profile.name = *info;
        }
    }

    // 5. Run the reset. Re-find the drive first (nodes may have changed).
    if (dry_run) {
        notify("IronKey dry run", "Setup collected for " + desc +
                                      (with_user ? " (admin + user)." : " (single password).") +
                                      " Nothing was sent to the drive.");
        wipe(admin);
        wipe(user);
        return;
    }
    notify("IronKey", "Resetting " + desc + "… this takes a few seconds.");
    UsbDevice fresh;
    if (!find_drive(port, fresh)) {
        wipe(admin);
        wipe(user);
        notify("IronKey", "The drive is no longer connected; nothing was changed.", true);
        return;
    }
    try {
        ResetOutcome o = reset_device_auto(fresh, *admin, user ? *user : std::string(), profile,
                                           std::nullopt, [](const std::string&) {});
        notify("IronKey set up",
               o.replug_needed
                   ? "Reset complete. Unplug and replug the drive, then unlock it to use it."
                   : "Reset complete. Unlock the drive from the tray menu to use it.");
    } catch (const std::exception& e) {
        notify("IronKey reset failed", e.what(), true);
    }
    wipe(admin);
    wipe(user);
}

}  // namespace ironkey
