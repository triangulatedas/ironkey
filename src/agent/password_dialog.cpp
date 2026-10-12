// SPDX-License-Identifier: GPL-2.0-only
#include "agent/password_dialog.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <sstream>
#include <vector>

#include "proto/bytes.h"

extern char** environ;

namespace ironkey {

namespace {

bool in_path(const std::string& name)
{
    const char* path = std::getenv("PATH");
    std::istringstream dirs(path ? path : "/usr/bin:/bin");
    std::string dir;
    while (std::getline(dirs, dir, ':'))
        if (!dir.empty() && ::access((dir + "/" + name).c_str(), X_OK) == 0)
            return true;
    return false;
}

std::string escape_markup(const std::string& s)
{
    std::string out;
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out += c;
        }
    }
    return out;
}

}  // namespace

std::string PasswordDialog::program()
{
    const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
    bool kde = desktop && std::string(desktop).find("KDE") != std::string::npos;
    const char* order[] = {kde ? "kdialog" : "zenity", kde ? "zenity" : "kdialog"};
    for (const char* p : order)
        if (in_path(p))
            return p;
    return {};
}

std::optional<std::string> PasswordDialog::ask(const std::string& title, const std::string& text)
{
    std::string prog = program();
    if (prog.empty())
        return std::nullopt;

    std::vector<std::string> args;
    if (prog == "zenity")
        args = {"zenity", "--entry", "--hide-text", "--width=420", "--title", title,
                "--text", escape_markup(text), "--ok-label", "Unlock"};
    else
        args = {"kdialog", "--title", title, "--password", text};

    std::vector<char*> argv;
    for (auto& a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);

    int pipefd[2];
    if (::pipe2(pipefd, O_CLOEXEC) < 0)
        return std::nullopt;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);

    // The agent blocks SIGTERM for its own signal handling; the dialog must
    // not inherit that, or cancel() could not close it.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t empty;
    sigemptyset(&empty);
    posix_spawnattr_setsigmask(&attr, &empty);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);

    pid_t pid;
    int rc;
    {
        std::lock_guard lock(mutex_);
        rc = posix_spawnp(&pid, argv[0], &actions, &attr, argv.data(), environ);
        if (rc == 0)
            pid_ = pid;
    }
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    ::close(pipefd[1]);
    if (rc != 0) {
        ::close(pipefd[0]);
        return std::nullopt;
    }

    std::string password;
    password.reserve(256);
    char buf[64];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR))
        if (n > 0)
            password.append(buf, static_cast<size_t>(n));
    secure_wipe(buf, sizeof buf);
    ::close(pipefd[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    {
        std::lock_guard lock(mutex_);
        pid_ = -1;
    }

    if (!password.empty() && password.back() == '\n')
        password.pop_back();
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        secure_wipe(password.data(), password.capacity());
        return std::nullopt;
    }
    return password;
}

void PasswordDialog::cancel()
{
    std::lock_guard lock(mutex_);
    if (pid_ > 0)
        ::kill(pid_, SIGTERM);
}

}  // namespace ironkey
