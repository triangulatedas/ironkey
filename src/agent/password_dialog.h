// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <sys/types.h>

#include <mutex>
#include <optional>
#include <string>

namespace ironkey {

// Graphical password prompt using zenity (GNOME and others) or kdialog
// (KDE). Runs the dialog as a child process and reads the password from its
// stdout, so no GUI toolkit is linked into the agent.
class PasswordDialog {
public:
    // Blocks until the user answers. Returns nullopt if the user cancelled,
    // no dialog program is installed, or cancel() was called.
    std::optional<std::string> ask(const std::string& title, const std::string& text);

    // Closes a running dialog. Safe to call from another thread.
    void cancel();

    // Name of the dialog program that will be used, or empty if none found.
    static std::string program();

private:
    std::mutex mutex_;
    pid_t pid_ = -1;
};

}  // namespace ironkey
