// SPDX-License-Identifier: GPL-2.0-only
#include "linux/volumes.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "proto/bytes.h"

extern char** environ;

namespace ironkey {

namespace {

// Runs a program without a shell; returns its exit status (-1 on failure).
// If `err` is given, it receives the program's stderr.
// TODO: talk to udisks over D-Bus directly instead.
int run(const std::vector<std::string>& args, std::string* err = nullptr)
{
    std::vector<char*> argv;
    for (const auto& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    int pipefd[2] = {-1, -1};
    if (err && ::pipe2(pipefd, O_CLOEXEC) < 0)
        return -1;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    if (err)
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
    else
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (err)
        ::close(pipefd[1]);
    if (rc != 0) {
        if (err)
            ::close(pipefd[0]);
        return -1;
    }

    if (err) {
        char buf[256];
        ssize_t n;
        while ((n = ::read(pipefd[0], buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR))
            if (n > 0)
                err->append(buf, static_cast<size_t>(n));
        ::close(pipefd[0]);
        while (!err->empty() && (err->back() == '\n' || err->back() == ' '))
            err->pop_back();
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// The block device and its partitions, e.g. /dev/sda, /dev/sda1.
std::vector<std::string> with_partitions(const std::string& node)
{
    std::vector<std::string> nodes{node};
    std::string name = std::filesystem::path(node).filename().string();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/block/" + name, ec)) {
        std::string part = entry.path().filename().string();
        if (part.starts_with(name) && std::filesystem::exists(entry.path() / "partition"))
            nodes.push_back("/dev/" + part);
    }
    return nodes;
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

// The mount point of `node` per /proc/self/mounts (octal-unescaped), or empty.
std::string mount_point_of(const std::string& node)
{
    std::ifstream mounts("/proc/self/mounts");
    std::string line;
    while (std::getline(mounts, line)) {
        std::istringstream fields(line);
        std::string source, target;
        fields >> source >> target;
        if (source != node)
            continue;
        std::string out;
        for (size_t i = 0; i < target.size(); ++i) {
            if (target[i] == '\\' && i + 3 < target.size()) {
                out += static_cast<char>((target[i + 1] - '0') * 64 + (target[i + 2] - '0') * 8 +
                                         (target[i + 3] - '0'));
                i += 3;
            } else {
                out += target[i];
            }
        }
        return out;
    }
    return {};
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

void power_off_drive(const UsbDevice& dev)
{
    if (dev.disks.empty())
        throw IronKeyError("No disk to power off on port " + dev.port);

    std::vector<std::string> nodes;
    for (const auto& d : dev.disks)
        for (const auto& n : with_partitions(d.node))
            nodes.push_back(n);
    for (const auto& cd : dev.cdroms)
        nodes.push_back(cd.node);

    for (const auto& node : nodes) {
        if (!is_mounted(node))
            continue;
        std::string err;
        if (run({"udisksctl", "unmount", "--no-user-interaction", "-b", node}, &err) != 0)
            throw IronKeyError("Could not unmount " + node +
                               (err.empty() ? "" : ": " + err) +
                               ". Close any files or windows using the drive and try again");
    }

    // Powering off the disk detaches the whole USB device; the drive locks
    // itself when it loses power.
    std::string err;
    if (run({"udisksctl", "power-off", "--no-user-interaction", "-b", dev.disks.front().node},
            &err) != 0)
        throw IronKeyError("Could not power off " + dev.disks.front().node +
                           (err.empty() ? "" : ": " + err));
}

std::string format_data_disk(const UsbDevice& dev, const std::string& fstype,
                             const std::string& label)
{
    const BlockDevice* disk = nullptr;
    for (const auto& d : dev.disks)
        if (d.size_bytes > 0) {
            disk = &d;
            break;
        }
    if (!disk)
        throw IronKeyError("No unlocked data disk to format. Unlock the drive first.");

    // Format replaces the whole device, so clear any mounts first.
    for (const auto& node : with_partitions(disk->node))
        if (is_mounted(node))
            run({"udisksctl", "unmount", "--no-user-interaction", "-b", node});

    std::string obj =
        "/org/freedesktop/UDisks2/block_devices/" + disk->node.substr(disk->node.rfind('/') + 1);

    sd_bus* bus = nullptr;
    int rc = sd_bus_open_system(&bus);
    if (rc < 0)
        throw IronKeyError(std::string("Cannot reach udisks (system bus): ") + std::strerror(-rc));

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* m = nullptr;
    sd_bus_message* reply = nullptr;
    auto cleanup = [&] {
        sd_bus_error_free(&error);
        sd_bus_message_unref(m);
        sd_bus_message_unref(reply);
        sd_bus_flush_close_unref(bus);
    };

    // Block.Format(type 's', options 'a{sv}'): no partition table, filesystem
    // on the whole device — the same as mkfs on /dev/sdX.
    rc = sd_bus_message_new_method_call(bus, &m, "org.freedesktop.UDisks2", obj.c_str(),
                                        "org.freedesktop.UDisks2.Block", "Format");
    if (rc >= 0)
        rc = sd_bus_message_append(m, "s", fstype.c_str());
    if (rc >= 0)
        rc = sd_bus_message_open_container(m, 'a', "{sv}");
    if (rc >= 0 && !label.empty())
        rc = sd_bus_message_append(m, "{sv}", "label", "s", label.c_str());
    if (rc >= 0 && fstype == "ext4")  // make the new fs owned by the invoking user
        rc = sd_bus_message_append(m, "{sv}", "take-ownership", "b", 1);
    if (rc >= 0)
        rc = sd_bus_message_close_container(m);
    if (rc >= 0)
        rc = sd_bus_call(bus, m, 0, &error, &reply);
    if (rc < 0) {
        std::string msg = error.message ? error.message : std::strerror(-rc);
        cleanup();
        throw IronKeyError("Format failed: " + msg);
    }
    cleanup();
    return disk->node;
}

bool write_file_to_data_disk(const std::string& node, const std::string& filename,
                             const std::string& contents)
{
    bool we_mounted = false;
    if (!is_mounted(node)) {
        if (run({"udisksctl", "mount", "--no-user-interaction", "-b", node}) != 0)
            return false;
        we_mounted = true;
    }
    std::string mp = mount_point_of(node);
    bool ok = false;
    if (!mp.empty()) {
        std::ofstream f(mp + "/" + filename, std::ios::binary | std::ios::trunc);
        f.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        f.flush();
        ok = f.good();
    }
    ::sync();
    if (we_mounted)
        run({"udisksctl", "unmount", "--no-user-interaction", "-b", node});
    return ok;
}

}  // namespace ironkey
