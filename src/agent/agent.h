// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "agent/password_dialog.h"
#include "linux/udev_monitor.h"

namespace ironkey {

// Tracks connected IronKeys and runs one unlock worker per USB port.
// All public methods are called from the main thread only.
class Agent {
public:
    // In a dry run the agent detects drives and shows the password dialog,
    // but never switches modes or sends anything to a drive. `Fail` pretends
    // every unlock failed, to exercise the retry dialog.
    enum class DryRun { Off, Succeed, Fail };

    explicit Agent(DryRun dry_run = DryRun::Off) : dry_run_(dry_run) {}
    ~Agent();

    // Starts workers for locked IronKeys that are already connected.
    void scan();
    void on_event(const UsbEvent& ev);
    // Forgets workers whose device has gone away.
    void reap();
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
    static void run(Worker& w, const std::string& port, DryRun dry_run);

    DryRun dry_run_;
    std::map<std::string, std::unique_ptr<Worker>> workers_;
};

}  // namespace ironkey
