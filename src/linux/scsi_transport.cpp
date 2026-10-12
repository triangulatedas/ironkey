// SPDX-License-Identifier: GPL-2.0-only
#include "linux/scsi_transport.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstring>

#include "proto/bytes.h"

namespace ironkey {

namespace {

bool read_all(int fd, void* buf, size_t n)
{
    auto* p = static_cast<uint8_t*>(buf);
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r <= 0)
            return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

bool write_all(int fd, const void* buf, size_t n)
{
    const auto* p = static_cast<const uint8_t*>(buf);
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r <= 0)
            return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

uint32_t rd32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24;
}
void wr32(uint8_t* p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

}  // namespace

ScsiTransport::ScsiTransport(const std::string& helper_path, const std::string& dev_node)
{
    int in_pipe[2];   // main -> helper stdin
    int out_pipe[2];  // helper stdout -> main
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0)
        throw IronKeyError("Could not create pipes for the SCSI helper");

    pid_ = fork();
    if (pid_ < 0)
        throw IronKeyError("Could not fork the SCSI helper");
    if (pid_ == 0) {
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        execl(helper_path.c_str(), helper_path.c_str(), dev_node.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    to_ = in_pipe[1];
    from_ = out_pipe[0];
}

ScsiTransport::~ScsiTransport()
{
    if (to_ >= 0) {
        uint8_t quit = 0;  // cdb_len == 0 tells the helper to exit
        write_all(to_, &quit, 1);
        close(to_);
    }
    if (from_ >= 0)
        close(from_);
    if (pid_ > 0) {
        int st = 0;
        waitpid(pid_, &st, 0);
    }
}

Bytes ScsiTransport::exchange(uint8_t sub, uint8_t cdb2, Direction dir, ByteView data,
                             uint32_t data_len, const Cdb& cdb, Bytes& status)
{
    // 12-byte vendor CDB: FF <sub> <cdb2> <cdb[0..7]> 00.
    uint8_t full_cdb[12] = {0xFF, sub, cdb2};
    std::copy(cdb.begin(), cdb.end(), full_cdb + 3);

    uint8_t req_dir = dir == Direction::Write ? 1 : dir == Direction::Read ? 2 : 0;
    uint32_t req_len = dir == Direction::Write ? static_cast<uint32_t>(data.size())
                       : dir == Direction::Read ? data_len
                                                : 0;

    uint8_t hdr[6];
    hdr[0] = sizeof full_cdb;  // cdb_len
    // write header pieces: cdb_len already in hdr[0]; send cdb, then dir+len.
    bool ok = write_all(to_, &hdr[0], 1) && write_all(to_, full_cdb, sizeof full_cdb);
    uint8_t dl[5];
    dl[0] = req_dir;
    wr32(&dl[1], req_len);
    ok = ok && write_all(to_, dl, sizeof dl);
    if (ok && req_dir == 1 && req_len)
        ok = write_all(to_, data.data(), req_len);
    if (!ok)
        throw IronKeyError("SCSI helper write failed (command " + std::to_string(sub) + ")");

    uint8_t rhdr[16];
    if (!read_all(from_, rhdr, sizeof rhdr))
        throw IronKeyError("SCSI helper closed (no device access? not setcap'd?)");
    uint8_t ioctl_ok = rhdr[0];
    uint8_t scsi_status = rhdr[1];
    uint32_t out_len = rd32(&rhdr[10]);

    Bytes result;
    if (out_len) {
        result.resize(out_len);
        if (!read_all(from_, result.data(), out_len))
            throw IronKeyError("SCSI helper truncated reply");
    }

    // Surface a device/transport failure as a non-zero status word so the
    // Channel raises DeviceStatusError, matching the HID status layout.
    if (!ioctl_ok) {
        status.assign(7, 0);
        status[4] = 0xFF;
        status[5] = 0xFF;
    } else if (scsi_status != 0) {
        status.assign(7, 0);
        status[4] = scsi_status;
    } else {
        status.clear();
    }
    return result;
}

}  // namespace ironkey
