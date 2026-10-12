// SPDX-License-Identifier: GPL-2.0-only
#include "linux/hidraw_device.h"

#include <fcntl.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <system_error>

namespace ironkey {

namespace {

// Every report on this device uses report ID 0, sent as a leading byte.
constexpr size_t kFeatureBufSize = kFeatureReportSize + 1;

[[noreturn]] void throw_errno(const std::string& what)
{
    throw std::system_error(errno, std::generic_category(), what);
}

}  // namespace

HidrawDevice::HidrawDevice(const std::string& path)
    : path_(path), fd_(::open(path.c_str(), O_RDWR | O_CLOEXEC))
{
    if (!fd_)
        throw_errno("open " + path);
}

void HidrawDevice::write_output(ByteView report)
{
    uint8_t buf[kOutputReportSize + 1] = {};
    std::copy_n(report.begin(), std::min(report.size(), kOutputReportSize), buf + 1);
    if (::write(fd_.get(), buf, sizeof buf) != static_cast<ssize_t>(sizeof buf))
        throw_errno("write " + path_);
}

std::optional<Bytes> HidrawDevice::read_input(std::chrono::milliseconds timeout)
{
    pollfd pfd{fd_.get(), POLLIN, 0};
    int rc;
    do {
        rc = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
    } while (rc < 0 && errno == EINTR);
    if (rc < 0)
        throw_errno("poll " + path_);
    if (rc == 0)
        return std::nullopt;

    Bytes buf(kOutputReportSize);
    ssize_t n = ::read(fd_.get(), buf.data(), buf.size());
    if (n < 0)
        throw_errno("read " + path_);
    buf.resize(n);
    return buf;
}

void HidrawDevice::set_feature(ByteView data)
{
    uint8_t buf[kFeatureBufSize] = {};
    std::copy_n(data.begin(), std::min(data.size(), kFeatureReportSize), buf + 1);
    if (::ioctl(fd_.get(), HIDIOCSFEATURE(kFeatureBufSize), buf) < 0)
        throw_errno("HIDIOCSFEATURE " + path_);
}

Bytes HidrawDevice::get_feature()
{
    uint8_t buf[kFeatureBufSize] = {};
    if (::ioctl(fd_.get(), HIDIOCGFEATURE(kFeatureBufSize), buf) < 0)
        throw_errno("HIDIOCGFEATURE " + path_);
    return Bytes(buf + 1, buf + kFeatureBufSize);
}

}  // namespace ironkey
