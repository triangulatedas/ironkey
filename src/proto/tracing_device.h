// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <functional>
#include <string>

#include "proto/hid_device.h"

namespace ironkey {

// Hex dump, 16 bytes per line. Trailing zero bytes are summarised so 512-byte
// feature reports stay readable.
std::string hex_dump(ByteView data, const std::string& indent = "    ");

// Wraps a HidDevice and logs every exchange, for protocol analysis.
// Feature data sent with FF 8F (PVC key) and FF A4 (password) is redacted.
class TracingHidDevice : public HidDevice {
public:
    using Sink = std::function<void(const std::string&)>;

    TracingHidDevice(HidDevice& inner, Sink sink) : inner_(inner), sink_(std::move(sink)) {}

    void write_output(ByteView report) override;
    std::optional<Bytes> read_input(std::chrono::milliseconds timeout) override;
    void set_feature(ByteView data) override;
    Bytes get_feature() override;

private:
    HidDevice& inner_;
    Sink sink_;
    uint8_t last_command_ = 0;
};

}  // namespace ironkey
