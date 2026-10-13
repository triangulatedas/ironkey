// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <string>

namespace ironkey {

// Runs the interactive reset / initialize wizard (a zenity dialog sequence) for
// the drive on `port`, then performs the reset. This blocks on user input, so
// call it on a background thread — never on the agent's event-loop thread. It is
// self-contained (captures `port` by value and uses only free device_ops/notify
// functions), so it is safe to run detached. With `dry_run`, it shows the whole
// dialog sequence but does NOT erase or write to the drive.
void run_setup_wizard(std::string port, bool dry_run);

// Unlocked-drive menu actions, each blocking (they drive a dialog and then a
// device operation), so run them on a background thread like the wizard.
// Open the drive's mounted volume in the file manager.
void open_data_disk(std::string port);
// Change a password in place (GTK dialog → change_password_device). Needs the
// current password; this is NOT the admin-resets-user operation.
void run_change_password_op(std::string port);
// Create a filesystem on the unlocked data disk (GTK dialog → format_data_disk).
void run_format_op(std::string port);

}  // namespace ironkey
