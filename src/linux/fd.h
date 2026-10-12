// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <unistd.h>

#include <utility>

namespace ironkey {

// Owns a file descriptor and closes it on destruction.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept
    {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { reset(); }

    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }

    void reset()
    {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_ = -1;
};

}  // namespace ironkey
