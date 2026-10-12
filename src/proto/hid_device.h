// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <chrono>
#include <optional>

#include "proto/bytes.h"

namespace ironkey {

constexpr size_t kOutputReportSize = 64;
constexpr size_t kFeatureReportSize = 512;

// Raw HID access to an IronKey in HID mode. Implemented over Linux hidraw
// in production and by fakes in tests. Report IDs are handled by the
// implementation; callers only see the payload.
class HidDevice {
public:
    virtual ~HidDevice() = default;

    // Send a 64-byte output report (command header).
    virtual void write_output(ByteView report) = 0;

    // Wait for a 64-byte input report from the interrupt endpoint.
    // Returns nullopt on timeout.
    virtual std::optional<Bytes> read_input(std::chrono::milliseconds timeout) = 0;

    // Send a feature report; data is zero-padded to 512 bytes.
    virtual void set_feature(ByteView data) = 0;

    // Read a 512-byte feature report.
    virtual Bytes get_feature() = 0;
};

}  // namespace ironkey
