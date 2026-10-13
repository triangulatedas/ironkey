// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <sys/types.h>

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "proto/session.h"  // Account

namespace ironkey {

// Graphical unlock prompt. Prefers the GTK front-end (ironkey-gui), which can
// offer an in-dialog admin/user toggle; falls back to zenity/kdialog for just
// the password. Either way it runs as a child process and reads the result from
// stdout, so no GUI toolkit is linked into the agent.
class PasswordDialog {
public:
    // The user's answer from ask_unlock().
    struct UnlockChoice {
        std::string password;
        Account account = Account::Admin;  // chosen (or forced/defaulted) account
    };

    // Blocks until the user answers. `admin_user` shows the account toggle (on a
    // drive with both accounts); `forced` pins the account and hides the toggle;
    // `note` is an optional warning line. Returns nullopt on cancel / no dialog /
    // cancel(). The GTK dialog is preferred; zenity is the fallback.
    std::optional<UnlockChoice> ask_unlock(const std::string& label, bool admin_user,
                                           std::optional<Account> forced, const std::string& note);

    // Plain password entry via zenity/kdialog. Returns nullopt as above.
    std::optional<std::string> ask(const std::string& title, const std::string& text);

    // Closes a running dialog. Safe to call from another thread.
    void cancel();

    // Name of the zenity/kdialog program that will be used, or empty if none.
    static std::string program();

private:
    struct Capture {
        bool spawned = false;
        int code = -1;
        std::string out;
    };
    // Spawns argv[0] with argv, tracking the pid so cancel() can close it.
    Capture run_capture(const std::vector<std::string>& argv);

    std::mutex mutex_;
    pid_t pid_ = -1;
};

}  // namespace ironkey
