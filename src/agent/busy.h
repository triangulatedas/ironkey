// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <atomic>
#include <mutex>
#include <set>
#include <string>

namespace ironkey {

// Ports a tray operation (reset, format, password change) is currently driving.
// Those operations re-enumerate the drive, and each re-appearance looks like a
// fresh plug-in; while a port is busy the agent must not start its own unlock
// prompt on it, and the tray must not read from it, or the two talk over each
// other on the same channel. Thread-safe: ops run on detached threads.
class BusyPorts {
public:
    static bool contains(const std::string& port)
    {
        std::lock_guard lock(mutex());
        return ports().contains(port);
    }

    // Bumped whenever a port becomes busy or free, so the tray can refresh.
    static unsigned generation() { return counter().load(); }

    // Marks `port` busy for the guard's lifetime. Returns an inactive guard
    // (owns() == false) if another operation already holds it.
    class Guard {
    public:
        explicit Guard(std::string port) : port_(std::move(port))
        {
            std::lock_guard lock(mutex());
            owns_ = ports().insert(port_).second;
            if (owns_)
                ++counter();
        }
        ~Guard()
        {
            if (!owns_)
                return;
            std::lock_guard lock(mutex());
            ports().erase(port_);
            ++counter();
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        bool owns() const { return owns_; }

    private:
        std::string port_;
        bool owns_ = false;
    };

private:
    static std::mutex& mutex()
    {
        static std::mutex m;
        return m;
    }
    static std::atomic<unsigned>& counter()
    {
        static std::atomic<unsigned> c{0};
        return c;
    }
    static std::set<std::string>& ports()
    {
        static std::set<std::string> p;
        return p;
    }
};

}  // namespace ironkey
