// SPDX-License-Identifier: GPL-2.0-only
#include "proto/channel.h"

#include <cstdio>
#include <thread>

namespace ironkey {

namespace {

constexpr uint32_t kMrotMagic = 0x4D524F54;

void put_le32(uint8_t* p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

std::string hex8(uint8_t v)
{
    char buf[8];
    std::snprintf(buf, sizeof buf, "0x%02X", v);
    return buf;
}

std::string hex16(uint16_t v)
{
    char buf[8];
    std::snprintf(buf, sizeof buf, "0x%04X", v);
    return buf;
}

}  // namespace

DeviceStatusError::DeviceStatusError(uint8_t command_, uint16_t status_)
    : IronKeyError("Command " + hex8(command_) + " failed: status=" + hex16(status_)),
      command(command_),
      status(status_)
{
}

std::array<uint8_t, kOutputReportSize> build_command_report(
    uint8_t sub, uint8_t cdb2, Direction dir, uint32_t data_len, const Cdb& cdb)
{
    std::array<uint8_t, kOutputReportSize> out{};
    out[0x00] = sub;
    out[0x01] = cdb2;
    put_le32(&out[0x02], data_len);
    put_le32(&out[0x06], data_len > 0 ? 1 : 0);  // chunk count
    put_le32(&out[0x0A], kMrotMagic);
    out[0x0E] = dir == Direction::Read ? 1 : 0;
    std::copy(cdb.begin(), cdb.end(), out.begin() + 0x10);
    return out;
}

Bytes Channel::command(uint8_t sub, uint8_t cdb2, Direction dir, ByteView data,
                       uint32_t data_len, const Cdb& cdb)
{
    uint32_t len = data_len ? data_len : static_cast<uint32_t>(data.size());
    dev_.write_output(build_command_report(sub, cdb2, dir, len, cdb));

    if (!dev_.read_input(response_timeout))
        throw IronKeyError("No acknowledgement for command " + hex8(sub));

    if (dir == Direction::Write && !data.empty()) {
        dev_.set_feature(data);
        check_status(sub, dev_.read_input(response_timeout));
        return {};
    }

    if (dir == Direction::Read && len > 0) {
        if (read_delay.count() > 0)
            std::this_thread::sleep_for(read_delay);
        Bytes result = dev_.get_feature();
        // The status that follows a read is not meaningful; consume it.
        dev_.read_input(response_timeout);
        if (result.size() > len)
            result.resize(len);
        return result;
    }

    check_status(sub, dev_.read_input(response_timeout));
    return {};
}

void Channel::drain()
{
    while (dev_.read_input(std::chrono::milliseconds(200)))
        ;
}

void Channel::check_status(uint8_t sub, const std::optional<Bytes>& resp)
{
    // A missing status report is tolerated, as in the original tool.
    if (!resp || resp->size() < 6)
        return;
    uint16_t status = (*resp)[4] | ((*resp)[5] << 8);
    if (status != 0)
        throw DeviceStatusError(sub, status);
}

}  // namespace ironkey
