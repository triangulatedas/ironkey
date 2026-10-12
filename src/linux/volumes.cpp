// SPDX-License-Identifier: GPL-2.0-only
#include "linux/volumes.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fstream>
#include <sstream>

extern char** environ;

namespace ironkey {

namespace {

// Runs a program without a shell; returns its exit status (-1 on failure).
// TODO: talk to udisks over D-Bus directly instead.
int run(const std::vector<std::string>& args)
{
    std::vector<char*> argv;
    for (const auto& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0)
        return -1;

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool is_mounted(const std::string& node)
{
    std::ifstream mounts("/proc/self/mounts");
    std::string line;
    while (std::getline(mounts, line)) {
        std::istringstream fields(line);
        std::string source;
        fields >> source;
        if (source == node)
            return true;
    }
    return false;
}

}  // namespace

std::vector<std::string> release_volumes(const UsbDevice& dev)
{
    std::vector<std::string> problems;
    for (const auto& cd : dev.cdroms) {
        if (is_mounted(cd.node) &&
            run({"udisksctl", "unmount", "--no-user-interaction", "-b", cd.node}) != 0)
            problems.push_back("could not unmount " + cd.node);
        // The original tool also ejected the CD; keep doing so.
        if (run({"eject", cd.node}) != 0)
            problems.push_back("could not eject " + cd.node);
    }
    return problems;
}

}  // namespace ironkey
