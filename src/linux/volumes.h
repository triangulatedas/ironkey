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

// Creates a filesystem on the drive's unlocked data disk via udisks (polkit, no
// root). `fstype` is "vfat", "exfat" or "ext4". DESTROYS the disk's contents.
// Returns the formatted node. Throws IronKeyError (with udisks' message) on
// failure, e.g. if not authorized or the disk is busy.
std::string format_data_disk(const UsbDevice& dev, const std::string& fstype,
                             const std::string& label);

// Mounts the data disk at `node` (if not already), writes `filename` with
// `contents`, syncs and unmounts. Best-effort — returns false on any failure.
// Used to drop the "USBtoCloudInstallDeclined.txt" marker after formatting so the
// Windows vendor client stops nagging about its cloud-backup upsell.
bool write_file_to_data_disk(const std::string& node, const std::string& filename,
                             const std::string& contents);

}  // namespace ironkey
