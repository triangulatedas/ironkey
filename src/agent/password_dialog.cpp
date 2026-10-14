// SPDX-License-Identifier: GPL-2.0-only
#include "agent/password_dialog.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

#include "agent/notify.h"
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

// $IRONKEY_GUI, else next to this executable, else "ironkey-gui" on PATH.
std::string find_gui()
{
    if (const char* e = std::getenv("IRONKEY_GUI"))
        return e;
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = '\0';
        std::string p(buf);
        auto slash = p.rfind('/');
        if (slash != std::string::npos)
            return p.substr(0, slash + 1) + "ironkey-gui";
    }
    return "ironkey-gui";
}

}  // namespace

bool PasswordDialog::gui_available()
{
    std::string gui = find_gui();
    return gui.find('/') != std::string::npos ? ::access(gui.c_str(), X_OK) == 0 : in_path(gui);
}

PasswordDialog::Capture PasswordDialog::run_capture(const std::vector<std::string>& args)
{
    Capture cap;
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    int pipefd[2];
    if (::pipe2(pipefd, O_CLOEXEC) < 0)
        return cap;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    // The agent blocks SIGTERM for its own handling; the dialog must not inherit
    // that, or cancel() could not close it.
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
        return cap;
    }

    cap.spawned = true;
    cap.out.reserve(256);
    char buf[128];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR))
        if (n > 0)
            cap.out.append(buf, static_cast<size_t>(n));
    secure_wipe(buf, sizeof buf);
    ::close(pipefd[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    {
        std::lock_guard lock(mutex_);
        pid_ = -1;
    }
    cap.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return cap;
}

std::optional<PasswordDialog::UnlockChoice> PasswordDialog::ask_unlock(const std::string& label,
                                                                       bool admin_user,
                                                                       std::optional<Account> forced,
                                                                       const std::string& note)
{
    Account def = forced ? *forced : (admin_user ? Account::User : Account::Admin);

    // Prefer the GTK front-end (ironkey-gui unlock).
    std::vector<std::string> args = {find_gui(), "unlock", "--label", label};
    if (admin_user && !forced) {
        args.push_back("--accounts");
        args.push_back("adminuser");
    }
    if (!note.empty()) {
        args.push_back("--note");
        args.push_back(note);
    }
    Capture cap = run_capture(args);
    if (cap.spawned && cap.code != 127) {
        if (cap.code != 0) {  // cancelled
            secure_wipe(cap.out.data(), cap.out.capacity());
            return std::nullopt;
        }
        UnlockChoice choice;
        choice.account = def;
        size_t pos = 0;
        while (pos < cap.out.size()) {
            size_t nl = cap.out.find('\n', pos);
            std::string line = cap.out.substr(pos, nl - pos);
            pos = (nl == std::string::npos) ? cap.out.size() : nl + 1;
            auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "password")
                choice.password = v;
            else if (k == "account")
                choice.account = (v == "admin") ? Account::Admin : Account::User;
        }
        secure_wipe(cap.out.data(), cap.out.capacity());
        return choice;
    }

    notify("IronKey", "The unlock dialog needs the ironkey-gui package. Install it, or use "
                      "`ironkey-unlock unlock`.",
           true);
    return std::nullopt;
}

void PasswordDialog::cancel()
{
    std::lock_guard lock(mutex_);
    if (pid_ > 0)
        ::kill(pid_, SIGTERM);
}

}  // namespace ironkey
