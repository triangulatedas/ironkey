// SPDX-License-Identifier: GPL-2.0-only
#include "proto/tracing_device.h"

#include <cstdio>

namespace ironkey {

namespace {

std::string hex2(uint8_t v)
{
    char buf[4];
    std::snprintf(buf, sizeof buf, "%02X", v);
    return buf;
}

}  // namespace

std::string hex_dump(ByteView data, const std::string& indent)
{
    size_t used = data.size();
    while (used > 0 && data[used - 1] == 0)
        --used;
    size_t shown = (used + 15) / 16 * 16;
    if (shown > data.size())
        shown = data.size();

    std::string out;
    for (size_t off = 0; off < shown; off += 16) {
        char prefix[32];
        std::snprintf(prefix, sizeof prefix, "%04zx: ", off);
        out += indent + prefix;
        for (size_t i = off; i < off + 16 && i < data.size(); ++i)
            out += hex2(data[i]) + (i % 16 == 7 ? "  " : " ");
        out += "\n";
    }
    if (shown < data.size())
        out += indent + "(" + std::to_string(data.size() - shown) + " trailing zero bytes)\n";
    if (data.empty())
        out += indent + "(empty)\n";
    return out;
}

void TracingHidDevice::write_output(ByteView report)
{
    last_command_ = report.empty() ? 0 : report[0];
    sink_("> command FF " + hex2(last_command_) + "\n" + hex_dump(report.first(32)));
    inner_.write_output(report);
}

std::optional<Bytes> TracingHidDevice::read_input(std::chrono::milliseconds timeout)
{
    auto r = inner_.read_input(timeout);
    if (r)
        sink_("< input\n" + hex_dump(*r));
    return r;
}

void TracingHidDevice::set_feature(ByteView data)
{
    if (last_command_ == 0x8F || last_command_ == 0xA4)
        sink_("> feature (" + std::to_string(data.size()) + " bytes, redacted)\n");
    else
        sink_("> feature\n" + hex_dump(data));
    inner_.set_feature(data);
}

Bytes TracingHidDevice::get_feature()
{
    Bytes r = inner_.get_feature();
    sink_("< feature\n" + hex_dump(r));
    return r;
}

}  // namespace ironkey
