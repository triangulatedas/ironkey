// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <sys/types.h>

#include <string>

#include "proto/channel.h"

namespace ironkey {

// Runs IronKey vendor commands over SG_IO by driving the setcap'd
// ironkey-scsi-helper process (one exchange per command). Staying on the SCSI
// storage path means a reset re-presents the data area in place, with no
// replug.
class ScsiTransport : public Transport {
public:
    // Spawns `helper_path <dev_node>` connected by pipes. Throws IronKeyError if
    // the helper cannot be started.
    ScsiTransport(const std::string& helper_path, const std::string& dev_node);
    ~ScsiTransport() override;
    ScsiTransport(const ScsiTransport&) = delete;
    ScsiTransport& operator=(const ScsiTransport&) = delete;

    Bytes exchange(uint8_t sub, uint8_t cdb2, Direction dir, ByteView data, uint32_t data_len,
                   const Cdb& cdb, Bytes& status) override;

private:
    int to_ = -1;    // write requests to the helper
    int from_ = -1;  // read replies from the helper
    pid_t pid_ = -1;
};

}  // namespace ironkey
