// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "agent/password_dialog.h"
#include "linux/udev_monitor.h"
#include "proto/session.h"

namespace ironkey {

// Tracks connected IronKeys and runs one unlock worker per USB port.
// All public methods are called from the main thread only.
class Agent {
public:
    // In a dry run the agent detects drives and shows the password dialog,
    // but never switches modes or sends anything to a drive. `Fail` pretends
    // every unlock failed, to exercise the retry dialog.
    enum class DryRun { Off, Succeed, Fail };

    // `account` forces admin or user login; by default it follows the drive's
    // password setup (user on admin+user drives).
    explicit Agent(DryRun dry_run = DryRun::Off, std::optional<Account> account = std::nullopt)
        : dry_run_(dry_run), account_(account)
    {
    }
    ~Agent();

    // Starts workers for locked IronKeys that are already connected.
    void scan();
    void on_event(const UsbEvent& ev);
    // Forgets workers whose device has gone away.
    void reap();
    // Starts (or restarts) the unlock flow for a drive on demand, e.g. from the
    // tray's Unlock item. No-op if a worker is already busy on that port.
    void request_unlock(const std::string& port);
    // Closes the drive's pending password prompt, if one is open: the user
    // picked another tray action (reset, change password, …) for it instead.
    void dismiss_prompt(const std::string& port);
    // Closes open dialogs and waits for all workers.
    void shutdown();

private:
    enum class State {
        Settling,   // waiting for the desktop to finish handling the new device
        Prompting,  // password dialog open
        Switching,  // HID mode switch; the device re-enumerates
        Unlocking,  // unlock commands; the device may re-enumerate
        Done,       // finished; stays until the device is unplugged
        Gone,       // device disappeared; can be forgotten
    };

    struct Worker {
        std::atomic<State> state{State::Settling};
        std::atomic<bool> cancelled{false};
        PasswordDialog dialog;
        std::thread thread;
    };

    void start(const std::string& port);
    void stop(const std::string& port);
    static void run(Worker& w, const std::string& port, DryRun dry_run,
                    std::optional<Account> account);

    DryRun dry_run_;
    std::optional<Account> account_;
    std::map<std::string, std::unique_ptr<Worker>> workers_;
};

}  // namespace ironkey
