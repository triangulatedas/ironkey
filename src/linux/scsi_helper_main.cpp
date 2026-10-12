// SPDX-License-Identifier: GPL-2.0-only
// ironkey-scsi-helper: a minimal privileged helper that does nothing but issue
// SG_IO on one device, on behalf of the unprivileged main tool.
//
// Sending the IronKey's vendor opcode (0xFF) through SG_IO needs CAP_SYS_RAWIO,
// which a udev rule cannot grant. Rather than give the whole tool that
// capability, this tiny program carries it (install with
// `setcap cap_sys_rawio+ep`) and only ever relays fixed-shape SG_IO requests.
//
// Protocol (binary, over stdin -> stdout), one request, one reply:
//   request:  cdb_len:u8, cdb[cdb_len], dir:u8 (0 none,1 to-dev,2 from-dev),
//             data_len:u32le, data[data_len] (only when dir==1)
//             cdb_len==0 means "quit".
//   reply:    ok:u8 (1 = ioctl ok), scsi_status:u8, host:u16le, driver:u16le,
//             resid:i32le, out_len:u32le, data[out_len] (only when dir==2)

#include <fcntl.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <vector>

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

int main(int argc, char** argv)
{
    if (argc < 2)
        return 2;
    int dev = open(argv[1], O_RDWR | O_NONBLOCK);
    if (dev < 0)
        return 3;

    for (;;) {
        uint8_t cdb_len = 0;
        if (!read_all(0, &cdb_len, 1) || cdb_len == 0)
            break;
        if (cdb_len > 16)
            cdb_len = 16;
        uint8_t cdb[16] = {0};
        if (!read_all(0, cdb, cdb_len))
            break;
        uint8_t dir = 0;
        uint8_t lenb[4] = {0};
        if (!read_all(0, &dir, 1) || !read_all(0, lenb, 4))
            break;
        uint32_t data_len = rd32(lenb);
        std::vector<uint8_t> data(data_len ? data_len : 1, 0);
        if (dir == 1 && data_len && !read_all(0, data.data(), data_len))
            break;

        sg_io_hdr_t io;
        std::memset(&io, 0, sizeof io);
        uint8_t sense[32] = {0};
        io.interface_id = 'S';
        io.cmd_len = cdb_len;
        io.cmdp = cdb;
        io.mx_sb_len = sizeof sense;
        io.sbp = sense;
        io.timeout = 20000;
        if (dir == 2) {
            io.dxfer_direction = SG_DXFER_FROM_DEV;
            io.dxfer_len = data_len;
            io.dxferp = data.data();
        } else if (dir == 1) {
            io.dxfer_direction = SG_DXFER_TO_DEV;
            io.dxfer_len = data_len;
            io.dxferp = data.data();
        } else {
            io.dxfer_direction = SG_DXFER_NONE;
        }

        int rc = ioctl(dev, SG_IO, &io);

        uint8_t rhdr[16] = {0};
        rhdr[0] = rc == 0 ? 1 : 0;
        rhdr[1] = io.status;
        rhdr[2] = io.host_status & 0xFF;
        rhdr[3] = (io.host_status >> 8) & 0xFF;
        rhdr[4] = io.driver_status & 0xFF;
        rhdr[5] = (io.driver_status >> 8) & 0xFF;
        wr32(&rhdr[6], static_cast<uint32_t>(io.resid));
        uint32_t out_len = 0;
        if (dir == 2) {
            int got = static_cast<int>(data_len) - io.resid;
            out_len = got > 0 ? static_cast<uint32_t>(got) : 0;
        }
        wr32(&rhdr[10], out_len);
        if (!write_all(1, rhdr, sizeof rhdr))
            break;
        if (out_len && !write_all(1, data.data(), out_len))
            break;
    }

    close(dev);
    return 0;
}
