// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <string>
#include <vector>

#include "linux/usb_topology.h"

namespace ironkey {

// Unmounts and ejects the device's own CD-ROM volumes (never anyone
// else's) so the mode switch can proceed. Uses udisks, so it works for the
// desktop user without root. Returns a description of anything that failed.
std::vector<std::string> release_volumes(const UsbDevice& dev);

// Unmounts every filesystem on the device (data partitions and CD-ROM), then
// asks udisks to power the drive off, which locks it. Same as "Eject" in a
// file manager. Throws IronKeyError (with udisks' message) if a filesystem is
// busy or the power-off fails.
void power_off_drive(const UsbDevice& dev);

}  // namespace ironkey
