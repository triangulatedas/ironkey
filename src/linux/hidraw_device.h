// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <string>

#include "linux/fd.h"
#include "proto/hid_device.h"

namespace ironkey {

// HidDevice over a Linux /dev/hidrawN node.
class HidrawDevice : public HidDevice {
public:
    explicit HidrawDevice(const std::string& path);

    void write_output(ByteView report) override;
    std::optional<Bytes> read_input(std::chrono::milliseconds timeout) override;
    void set_feature(ByteView data) override;
    Bytes get_feature() override;

    const std::string& path() const { return path_; }

private:
    std::string path_;
    Fd fd_;
};

}  // namespace ironkey
