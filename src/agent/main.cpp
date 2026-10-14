// SPDX-License-Identifier: GPL-2.0-only
// ironkey-agent: runs in the user's desktop session, notices IronKeys being
// plugged in and asks for the password to unlock them.
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string_view>

#include <thread>

#include "agent/agent.h"
#include "agent/busy.h"
#include "agent/notify.h"
#include "agent/password_dialog.h"
#include "agent/tray.h"
#include "agent/wizard.h"
#include "linux/device_ops.h"
#include "linux/udev_monitor.h"
#include "linux/usb_topology.h"

using namespace ironkey;

int main(int argc, char** argv)
{
    auto dry_run = Agent::DryRun::Off;
    std::optional<Account> account;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--dry-run") {
            dry_run = Agent::DryRun::Succeed;
        } else if (arg == "--dry-run=fail") {
            dry_run = Agent::DryRun::Fail;
        } else if (arg == "--user") {
            account = Account::User;
        } else if (arg == "--admin") {
            account = Account::Admin;
        } else if (arg == "-h" || arg == "--help") {
            std::puts("Usage: ironkey-agent [--admin | --user] [--dry-run | --dry-run=fail]\n\n"
                      "Waits for Kingston IronKey drives to be plugged in and asks for the\n"
                      "password to unlock them. Normally started by systemd as a user service.\n\n"
                      "  --admin, --user\n"
                      "             Which password to ask for on drives set up with admin\n"
                      "             and user passwords. Default: user, as the vendor app.\n"
                      "             Drives with a single password need neither.\n"
                      "  --dry-run  Detect drives and show the password dialog, but never\n"
                      "             switch modes or send anything to a drive.\n"
                      "  --dry-run=fail\n"
                      "             As --dry-run, but pretend every unlock failed, to test\n"
                      "             the retry dialog.");
            return 0;
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    // Handle SIGINT/SIGTERM through a signalfd. Blocked before any threads
    // start, so every thread inherits the mask.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (sfd < 0) {
        std::fprintf(stderr, "signalfd: %s\n", std::strerror(errno));
        return 1;
    }

    try {
        // Monitor first, then scan, so no device slips between the two.
        UdevMonitor monitor;
        Agent agent(dry_run, account);
        std::fprintf(stderr, "ironkey-agent started%s (login: %s, dialogs: %s)\n",
                     dry_run == Agent::DryRun::Off    ? ""
                     : dry_run == Agent::DryRun::Fail ? " in DRY RUN (fail) mode"
                                                      : " in DRY RUN mode",
                     account ? to_string(*account) : "by drive setup",
                     PasswordDialog::gui_available() ? "ironkey-gui"
                                                     : "none - install ironkey-gui");
        agent.scan();

        // System-tray icon (StatusNotifierItem + dbusmenu over sd-bus). If no
        // tray host is available the agent just runs headless as before.
        bool quit = false;
        Tray tray({
            .list =
                [] {
                    std::vector<Tray::Drive> out;
                    for (auto& d : find_kingston_devices()) {
                        if (!d.is_ironkey())
                            continue;
                        // Mid-operation: reading it would talk over the op.
                        if (BusyPorts::contains(d.port)) {
                            out.push_back({d.port, describe(d), d.is_unlocked(), false,
                                           "Working… please wait", true});
                            continue;
                        }
                        // One non-disruptive read per drive (SCSI when locked in
                        // storage mode); the host asks only on menu open.
                        DeviceSummary s = read_device_summary(d);
                        std::string label = s.owner_name.empty() ? describe(d) : s.owner_name;
                        bool admin_user = s.setup == PasswordSetup::AdminAndUser;
                        std::string status;
                        if (s.status_read) {
                            status = s.setup == PasswordSetup::AdminAndUser ? "Admin + user"
                                     : s.setup == PasswordSetup::Single      ? "Single password"
                                                                             : "Setup unknown";
                            if (s.admin_failed_attempts)
                                status += "  ·  " + std::to_string(*s.admin_failed_attempts) +
                                          " of " + std::to_string(kDefaultMaxFailedAttempts) +
                                          " failed logins";
                        }
                        out.push_back({d.port, label, d.is_unlocked(), admin_user, status});
                    }
                    return out;
                },
            .unlock = [&agent](std::string port) { agent.request_unlock(port); },
            .lock =
                [](std::string port) {
                    for (auto& d : find_kingston_devices())
                        if (d.is_ironkey() && d.port == port && d.is_unlocked()) {
                            try {
                                lock_device(d, [](const std::string&) {});
                            } catch (const std::exception& e) {
                                std::fprintf(stderr, "tray: lock failed: %s\n", e.what());
                            }
                            return;
                        }
                },
            .show_hint =
                [](std::string port) {
                    for (auto& d : find_kingston_devices()) {
                        if (!d.is_ironkey() || d.port != port)
                            continue;
                        Profile p = read_profile_auto(d);  // non-disruptive
                        std::string hint = !p.admin_hint.empty() ? p.admin_hint : p.user_hint;
                        notify("IronKey password hint",
                               hint.empty() ? "No password hint is stored on this drive." : hint);
                        return;
                    }
                },
            .setup =
                [&agent, dry = dry_run != Agent::DryRun::Off](std::string port) {
                    agent.dismiss_prompt(port);
                    // The wizard blocks on its dialog, so it must not run on
                    // the event-loop thread. It is self-contained, so detach it.
                    std::thread(run_setup_wizard, std::move(port), dry).detach();
                },
            .open = [](std::string port) { std::thread(open_data_disk, std::move(port)).detach(); },
            .change_pw =
                [&agent](std::string port) {
                    agent.dismiss_prompt(port);
                    std::thread(run_change_password_op, std::move(port)).detach();
                },
            .format = [](std::string port) { std::thread(run_format_op, std::move(port)).detach(); },
            .reset_user_pw =
                [&agent](std::string port) {
                    agent.dismiss_prompt(port);
                    std::thread(run_reset_user_password_op, std::move(port)).detach();
                },
            .quit = [&quit] { quit = true; },
        });
        tray.start();

        unsigned busy_seen = BusyPorts::generation();
        for (;;) {
            pollfd fds[3] = {{monitor.fd(), POLLIN, 0}, {sfd, POLLIN, 0}, {-1, 0, 0}};
            nfds_t nfds = 2;
            if (tray.active()) {
                fds[2] = {tray.fd(), static_cast<short>(tray.events()), 0};
                nfds = 3;
            }
            int rc = ::poll(fds, nfds, 1000);
            if (rc < 0 && errno != EINTR)
                throw std::runtime_error(std::string("poll: ") + std::strerror(errno));
            if (fds[1].revents & POLLIN)
                break;
            bool devices_changed = false;
            if (fds[0].revents & POLLIN)
                while (auto ev = monitor.receive()) {
                    agent.on_event(*ev);
                    if (ev->vid == kKingstonVid)
                        devices_changed = true;
                }
            agent.reap();
            if (unsigned g = BusyPorts::generation(); g != busy_seen) {
                busy_seen = g;
                devices_changed = true;  // an operation started or finished
            }
            if (tray.active()) {
                tray.process();  // serve menu/property calls, run menu actions
                if (devices_changed)
                    tray.notify_changed();
                if (quit)
                    break;
            }
        }
        std::fprintf(stderr, "ironkey-agent stopping\n");
        agent.shutdown();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ironkey-agent: %s\n", e.what());
        return 1;
    }
    return 0;
}
