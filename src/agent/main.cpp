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

#include "agent/agent.h"
#include "agent/password_dialog.h"
#include "linux/udev_monitor.h"

using namespace ironkey;

int main(int argc, char** argv)
{
    auto dry_run = Agent::DryRun::Off;
    auto account = Account::Admin;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--dry-run") {
            dry_run = Agent::DryRun::Succeed;
        } else if (arg == "--dry-run=fail") {
            dry_run = Agent::DryRun::Fail;
        } else if (arg == "--user") {
            account = Account::User;
        } else if (arg == "-h" || arg == "--help") {
            std::puts("Usage: ironkey-agent [--user] [--dry-run | --dry-run=fail]\n\n"
                      "Waits for Kingston IronKey drives to be plugged in and asks for the\n"
                      "password to unlock them. Normally started by systemd as a user service.\n\n"
                      "  --user     Log in with the user password (drives with admin and\n"
                      "             user passwords). Default: admin, or the\n"
                      "             only password on single-password drives.\n"
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
        std::string dialog = PasswordDialog::program();
        std::fprintf(stderr, "ironkey-agent started%s (%s login, password dialog: %s)\n",
                     dry_run == Agent::DryRun::Off    ? ""
                     : dry_run == Agent::DryRun::Fail ? " in DRY RUN (fail) mode"
                                                      : " in DRY RUN mode",
                     to_string(account),
                     dialog.empty() ? "none found - install zenity or kdialog" : dialog.c_str());
        agent.scan();

        pollfd fds[] = {{monitor.fd(), POLLIN, 0}, {sfd, POLLIN, 0}};
        for (;;) {
            int rc = ::poll(fds, 2, 1000);
            if (rc < 0 && errno != EINTR)
                throw std::runtime_error(std::string("poll: ") + std::strerror(errno));
            if (fds[1].revents & POLLIN)
                break;
            if (fds[0].revents & POLLIN)
                while (auto ev = monitor.receive())
                    agent.on_event(*ev);
            agent.reap();
        }
        std::fprintf(stderr, "ironkey-agent stopping\n");
        agent.shutdown();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ironkey-agent: %s\n", e.what());
        return 1;
    }
    return 0;
}
