// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "proto/hid_device.h"

namespace ironkey {

enum class Direction { None, Write, Read };

using Cdb = std::array<uint8_t, 8>;

// The device reported a non-zero status for a command.
class DeviceStatusError : public IronKeyError {
public:
    DeviceStatusError(uint8_t command_, uint16_t status_);
    uint8_t command;
    uint16_t status;
};

// Builds the 64-byte "MROT" command header (see PROTOCOL.md, Step 2).
std::array<uint8_t, kOutputReportSize> build_command_report(
    uint8_t sub, uint8_t cdb2, Direction dir, uint32_t data_len, const Cdb& cdb);

// An alternative command transport (the SCSI/SG_IO path) used instead of the
// HID report dance. One call = one vendor command. `status` is filled in the
// HID status layout ([4..5] = status word) so the Channel treats both the
// same.
class Transport {
public:
    virtual ~Transport() = default;
    virtual Bytes exchange(uint8_t sub, uint8_t cdb2, Direction dir, ByteView data,
                           uint32_t data_len, const Cdb& cdb, Bytes& status) = 0;
    virtual void drain_input() {}
};

// Sends IronKey vendor commands, over HID (the default) or a Transport (SCSI).
class Channel {
public:
    explicit Channel(HidDevice& dev) : dev_(&dev) {}
    explicit Channel(Transport& transport) : transport_(&transport) {}

    // Send a command. For Direction::Write, `data` goes out as a feature report;
    // `data_len` overrides the length in the header when non-zero.
    // For Direction::Read, returns `data_len` bytes read from the device.
    Bytes command(uint8_t sub, uint8_t cdb2, Direction dir, ByteView data = {},
                  uint32_t data_len = 0, const Cdb& cdb = {});

    // Discard any pending input reports.
    void drain();

    // Delay between sending a read command and fetching its feature report.
    // The device needs time to prepare the data; tests set this to zero.
    std::chrono::milliseconds read_delay{300};
    std::chrono::milliseconds response_timeout{2000};

    // The status report (interrupt input) that ended the last command, or
    // empty if none arrived. Layout: [2] command echo, [4..5] status,
    // [6] command-specific result (for FF A4: 1 = wrong password).
    Bytes last_status;

private:
    void check_status(uint8_t sub, std::optional<Bytes> resp);

    HidDevice* dev_ = nullptr;
    Transport* transport_ = nullptr;
};

}  // namespace ironkey
