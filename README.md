# ironkey

Unlock, reset, format and manage Kingston IronKey Locker+ G2 encrypted USB
drives on Linux — without root (for the common path) and without Python.

Kingston only ships management software for Windows and macOS. This project
began as a C++ rewrite of
[ironkey-unlocker](https://github.com/wltechblog/ironkey-unlocker), the Python
tool that first made these drives *unlockable* on Linux, and grew into a fuller
management tool: forgot-password reset, passphrase mode, owner-info read/write,
filesystem creation, and an optional replug-free SCSI path — each
reverse-engineered for this project (unlock's byte-level protocol is in
[PROTOCOL.md](PROTOCOL.md); the newer findings are described below and in the
commit history).

> This tool does not let anyone read the data on a lost or stolen *locked*
> drive without its password — the at-rest encryption is unaffected. It gives
> Linux users the same legitimate access the vendor software gives elsewhere.
> Along the way it did surface real weaknesses in the drive's *session*
> handshake (weak RSA and broken key generation); see
> [Security findings](#security-findings). Those matter only to an attacker who
> can record USB traffic while you unlock, not to one holding a locked drive.

## The original project

[ironkey-unlocker](https://github.com/wltechblog/ironkey-unlocker) was written
by Josh at [WLTechBlog](https://github.com/wltechblog) after Kingston sent a
review unit that turned out to have no Linux support. Over about two weeks the
proprietary unlock protocol was reverse-engineered:

- **USB captures** of the Windows and macOS applications in Wireshark.
- **Ghidra** decompilation of the macOS application, which uncovered the
  "PID switch" that exposes a hidden HID interface, the HID command format,
  the login call chain and the `encryptPVCKey` algorithm.
- **LLDB** live debugging on macOS to confirm the crypto with real
  intermediate values and to capture the correct `FF A4` parameters.
- **Re-analysis of the captures**, which found the missing `FF 89` commit
  command, the final piece needed to unlock.

The result was a single Python script, `ironkey_unlock.py`, run with `sudo`:

1. Find a Kingston device whose product name contains "IronKey" or "Locker".
2. Unmount the device's virtual CD-ROM and read a magic sequence of USB string
   descriptors, so that the drive re-enumerates with a HID interface.
3. Run an RSA-512 key exchange with the device to agree a 16-byte AES session
   key.
4. Send the encrypted PVC key and the AES-encrypted password, then commit.
5. Wait for the encrypted data partition to appear.

The full byte-level protocol is documented in [PROTOCOL.md](PROTOCOL.md),
copied from the original project. All credit for the reverse engineering goes
to the original author. This project changes only how the protocol is packaged
and used.

## What this version changes

### Goals

| Original | This version |
|---|---|
| Run by hand with `sudo python3 ironkey_unlock.py` | A session agent notices the drive being plugged in and asks for the password |
| Assumes exactly one IronKey is connected | Handles several IronKeys at once, each tracked separately |
| Needs Python 3 and `pycryptodome` | Compiled C++20 binaries, depending only on OpenSSL, libudev and libsystemd |
| Needs root | Runs as the logged-in user, through a udev `uaccess` rule |
| Copy the script and install dependencies | Debian package *(planned)* |

### Running without root

The original script needed root only to open the raw USB node
(`/dev/bus/usb/…`) and the hidraw node. [`packaging/70-ironkey.rules`](packaging/70-ironkey.rules)
tags just those IronKey nodes with `uaccess`. systemd-logind then grants
access to the user at the local seat, so the whole unlock runs unprivileged.
Mounted volumes are released through udisks instead of `umount`.

This also means the password never has to be handed to a privileged daemon.
The process that asks for it is the one that talks to the device.

The one exception is the optional **SCSI path** (`reset --scsi`): sending the
vendor opcode `0xFF` over `SG_IO` requires `CAP_SYS_RAWIO`, which a udev rule
cannot grant. That path uses a tiny `ironkey-scsi-helper` installed with
`setcap cap_sys_rawio+ep`; it does nothing but relay `SG_IO`, so the main tool
and the password still stay unprivileged. Everything else — unlock, lock,
change-password, reset over HID, info, format — needs no setcap.

### Multiple devices

The Python script always used the *first* match it found: the first Kingston
hidraw node, the first Kingston `sd*` disk, and `/dev/sr0`. With two drives
connected, it could switch, unlock or report on the wrong one. Any already
unlocked Kingston disk also made it report "already unlocked".

This version identifies each drive by its **USB port path** (for example
`2-1`). The port stays the same when the drive re-enumerates with a new PID
after the mode switch and after unlocking. For each drive, its own hidraw,
CD-ROM and disk nodes are found by walking the sysfs device tree under that
port, and only those nodes are touched. If several drives are connected,
`--device PORT` picks one, and the tool never guesses.

### Behaviour fixes found during the port

- **Kernel driver "detach" was a no-op.** The script used ioctl number
  `0x81015505` to detach `usb-storage` before the mode switch. That number
  decodes to `_IOR('U', 5, <257 bytes>)`, not `USBDEVFS_DISCONNECT`
  (`_IO('U', 22)`), so the kernel always rejected it and the error was
  silently ignored. Since the switch works anyway, this version leaves the
  detach out and documents why.
- **Eject touched the wrong drive.** The script ran `eject /dev/sr0`
  regardless of which device was being unlocked. This version ejects only the
  CD-ROM that belongs to the selected drive.
- **Unmount before permission check.** Volumes are now released only after
  confirming that the device can actually be opened.
- **Overlong passwords.** A password over 16 bytes is rejected *before*
  anything is sent to the device. Previously two session commands had already
  been sent when the error was raised.
- **Password on the command line.** The `-p PASSWORD` option is removed, since
  it exposes the password in `ps` and shell history. `unlock` prompts on the
  terminal with echo off, or reads from `--password-stdin`.
- **Password asked first.** The CLI and the agent ask for the password before
  switching the drive to HID mode, so cancelling leaves the drive untouched.
- **Wrong passwords were never detected.** The original checked the status
  field for errors, but the drive returns status OK even for a wrong password.
  The real result is in a separate byte of the status report. This version
  reads it and reports a wrong password immediately, along with the drive's
  failed-login count, which the original never read.
- **Secrets in memory.** The host RSA private key, the session key, the
  password and intermediate values are wiped with `OPENSSL_cleanse` after use.

For **unlock**, the protocol is unchanged: the same commands, parameters and
crypto, byte for byte ([PROTOCOL.md](PROTOCOL.md)). The other operations
(reset, passphrase, owner info, format, the SCSI path) are new to this project
and were reverse-engineered separately — see the Status section.

### Verified against the original

The C++ protocol code is tested against the Python implementation directly.
[`tools/gen_vectors.py`](tools/gen_vectors.py) loads the original
`ironkey_unlock.py`, replaces its I/O and randomness with fixed inputs, and
records:

- command framing for each command type,
- `clamp` and `encrypt_pvc_key` outputs,
- a complete handshake + unlock transcript: every output and feature report
  written, plus the derived session key.

The C++ tests replay the same inputs and must produce identical bytes. A
second test runs the full exchange against a simulated device that performs
the device side of the protocol with real RSA keys, including a
wrong-password case.

On real hardware (Locker+50G2), the following have been verified running as
a normal user: the mode switch, the RSA handshake, a full unlock with the
admin password, wrong-password detection with the failed-login counter, and
the agent's hotplug handling in `--dry-run` mode.

## Status

| Feature | State |
|---|---|
| Unlock (admin / user password, or passphrase) | done, hardware-verified |
| Lock (unmount + power off, which re-locks the drive) | done, hardware-verified |
| Multi-device (several IronKeys at once, matched by USB port) | done |
| Session agent (auto-prompt on insert, notifications) | done; hotplug verified, agent unlock path verified |
| Failed-login counter shown before/after unlock (admin account) | done |
| Change password in place (`FF A6` / `FF A8`) | done, hardware-verified |
| Reset / forgot-password (crypto-erase + set new password) | done, hardware-verified |
| Provision admin + user accounts (`reset --admin-user`) | done, hardware-verified |
| Passphrase mode (up to 64 characters) | done, hardware-verified |
| Owner info — read & write name / company / hints (`info`, `reset --…`) | done, hardware-verified |
| Create a filesystem on the data disk (`format`, vfat/exfat/ext4) | done, hardware-verified |
| SCSI transport + replug-free reset (privileged helper) | done, hardware-verified |
| Debian package | todo |
| CI builds for amd64 / arm64 | todo |

Several features beyond the original unlock were reverse-engineered for this
project, from the macOS vendor binary (which ships with full C++ symbols) and
fresh USB captures:

**Reset / forgot-password** (`ironkey-unlock reset`) wipes the drive and sets a
brand-new password without needing the old one. The password is set by the
`FF A2` *Configure* token (the password AES-encrypted under the session key,
with the re-key flag for the crypto-erase) — `FF A4` is only a login, never a
setter; three fixed `FF 07` region-prep commands restore the partition
geometry. The drive applies the new layout on a **power cycle**. Over the
default HID path that means one physical unplug/replug after `reset`; with the
SCSI path (below) it is done in software, so the whole flow is hands-off.

**Passphrase mode** — passwords may be up to 64 characters. The 16-byte value
the device verifies is derived exactly as the vendor app does (reverse-
engineered from `IronKey.app` and hardware-verified): **≤ 16 chars** are the raw
UTF-8 bytes zero-padded to 16; **17–64 chars** are folded to 16 bytes with
`MD5(utf8(password))`, so the *whole* passphrase counts. The set token and the
login block use the same derivation, so provisioning and login agree and both
interoperate with the vendor software.

**Admin + user accounts** (`ironkey-unlock reset --admin-user`) provisions two
separate passwords on the drive. The device infers the account count from the
`FF A2` payload length: `size[4]` + one 16-byte token is a single (admin)
account; `size[4]` + two tokens (admin first, then user) provisions both. The
CDB is identical either way. Verified on hardware — after provisioning, the
drive reports both accounts and each password unlocks the data disk
independently (the user account can be reset by the admin, and ten failed admin
logins still crypto-erase the whole drive).

**Owner info** — the drive stores a contact profile (name, company,
description, and the admin/user password *hints*) in its protected hidden area,
AES-ECB-encrypted under a fixed key and reachable through the factory session.
`ironkey-unlock info` prints it **without unlocking**; `reset --name … --company
… --admin-hint …` writes it. Note the security implication: the hints travel
with the drive and are readable before unlock on any machine.

**Format** — `ironkey-unlock format --fs vfat|exfat|ext4 [--label NAME]`
creates a filesystem on an unlocked drive's data disk through udisks (polkit,
no root); `ext4` is made owned by the invoking user.

**SCSI transport (replug-free reset)** — the vendor commands can run over
`SG_IO` instead of HID, keeping the drive in storage mode. That lets `reset`
issue `FF 01` (a soft power-cycle) to re-enumerate the drive in software, so no
physical replug is needed: `reset` → `unlock` → `format`, hands-off. `reset`
selects this path automatically when the helper is available. Sending opcode `0xFF` over `SG_IO` needs `CAP_SYS_RAWIO`,
which a udev rule cannot grant, so this path uses a tiny privileged helper
(`ironkey-scsi-helper`, installed with `setcap cap_sys_rawio+ep`) that does
nothing but relay `SG_IO`; the main tool stays unprivileged. HID remains the
zero-setup default.

## Building

Requires CMake ≥ 3.20, a C++20 compiler, OpenSSL 3, libudev and libsystemd
(sd-bus, for notifications):

```bash
sudo apt install build-essential cmake pkg-config libssl-dev libudev-dev libsystemd-dev
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Usage

Until the Debian package exists, install from a build:

```bash
sudo cmake --install build --prefix /usr     # binaries, udev rule, systemd user unit
sudo udevadm control --reload                # then replug the drive
systemctl --user enable --now ironkey-agent  # start the agent with your desktop session
```

### Agent

`ironkey-agent` runs in your desktop session. When an IronKey is plugged in, it
shows a password dialog (zenity, or kdialog on KDE). After unlocking, the drive
is mounted by the desktop as usual and a notification shows which disk it is.

- Each drive gets its own dialog, naming the model, serial and USB port.
- Cancel leaves the drive locked. The agent doesn't ask again until the drive
  is replugged.
- Unplugging the drive closes its dialog.
- The "unlocked" notification says how to lock the drive again (Eject in
  Files, or `ironkey-unlock lock`).
- After a failed attempt the dialog comes back with the failure count and a
  warning. Passwords are never retried automatically.
- `ironkey-agent --user` logs in with the user password on drives set up with
  admin and user passwords. The default is the admin password, or the only
  password on single-password drives.
- `ironkey-agent --dry-run` detects drives and shows the dialog, but never
  sends anything to a drive. Use it for testing.

Logs: `journalctl --user -u ironkey-agent`.

### Command line

```bash
ironkey-unlock list                        # connected Kingston devices, state, nodes and access
ironkey-unlock unlock [--device PORT]      # prompt for the password and unlock
ironkey-unlock unlock --user               # log in with the user password (admin+user drives)
ironkey-unlock unlock --password-stdin     # read the password from stdin (scripts)
ironkey-unlock change-password             # change a password in place (admin or --user)
ironkey-unlock reset                       # forgot-password: ERASE and set a new password
ironkey-unlock reset --name N --company C --admin-hint H   # also store owner info
ironkey-unlock reset --scsi / --hid        # force the transport (default: auto)
ironkey-unlock info                        # show owner info (name/company/hints), no unlock
ironkey-unlock format --fs ext4 --label X  # create a filesystem on the unlocked data disk
ironkey-unlock lock [--device PORT]        # unmount and power off, which locks the drive
ironkey-unlock hid-mode  [--device PORT]   # switch a locked drive to HID mode
ironkey-unlock handshake [--device PORT]   # diagnostic: key exchange only, no password sent
```

`--device PORT` is needed only when more than one IronKey is connected.

`reset` automatically uses the **replug-free SCSI path** when the privileged
helper is installed and the drive is in storage mode (it preflights a harmless
read), and otherwise falls back to **HID**, where you unplug/replug the drive
once before `unlock` and `format` (the device applies its new layout on a power
cycle). `--scsi` and `--hid` force the choice. See the SCSI section under
Status.

`lock` does the same as **Eject** in a file manager. It unmounts the drive's
data partitions and CD-ROM, then asks udisks to power it off. The drive locks
itself when it loses power, and writes are flushed first, so it is safe to
unplug afterwards. If a file or window still uses the drive, the command stops
with udisks' "target is busy" message and the drive stays unlocked.
`unlock` exits with code 3 if the drive did not unlock.

Example:

```
$ ironkey-unlock list
port 2-1  0951:159d  Locker+50G2  S/N 80C5F260C660BA70F1F32591  [locked]
    usb     /dev/bus/usb/002/007 (rw)
    cdrom   /dev/sr0 (0.1 GiB)
    disk    /dev/sda (empty)
```

> IronKey drives erase themselves after too many wrong passwords. Neither the
> CLI nor the agent ever retries a password automatically.

## Hardware notes

Tested with a Locker+50G2 (64 GB): locked PID `0951:159d`, HID mode PID
`0951:00d3`. Unlocking with the admin password has been verified on hardware.
The drive reports wrong passwords and keeps a failed-login counter that can
be read without logging in; both tools show it (see
[PROTOCOL.md](PROTOCOL.md#status-queries-ff-21-ff-a0)). With separate admin and
user passwords set, `--admin` (default, index 0) and `--user` (index 1) both
unlock on hardware. The default is admin because the tools can't yet tell a
single-password drive (where index 0 is the only password) from an admin+user
one.
The drive keeps its USB port path and serial across the switch. Other
capacities (16G2, 32G2, …) use other PIDs. Devices are recognised by vendor ID
and product name, so they are expected to work, but have not been tested.

## Layout

```
src/proto/   Protocol: command framing, RSA handshake, unlock. Depends only on OpenSSL.
src/linux/   Linux device access: hidraw, usbfs mode switch, sysfs topology, volumes.
src/cli/     ironkey-unlock command.
src/agent/   ironkey-agent: udev monitor, per-port workers, password dialog, notifications.
packaging/   udev rule and systemd user unit.
tests/       Unit tests; vectors.inc is generated from the original Python code.
tools/       gen_vectors.py
```

To regenerate the test vectors (needs `python3-cryptography`, not
`pycryptodome`):

```bash
python3 tools/gen_vectors.py /path/to/ironkey-unlocker/ironkey_unlock.py > tests/vectors.inc
```

## Security findings

Implementing the protocol faithfully meant looking closely at the drive's
session handshake, and it has serious weaknesses. To be clear about scope
first: **none of this breaks the drive's at-rest encryption.** A lost or
stolen *locked* drive is still safe — there is no handshake to attack when the
drive is locked. These weaknesses matter only to an attacker who can
**passively record USB traffic while you unlock or set a password** (a
compromised or untrusted host). Within that threat model, they are severe.

**1. The session handshake uses RSA-512.** Every unlock, and every
password-setting operation, negotiates the 16-byte AES session key under
ephemeral RSA-512 keys, then sends your password AES-encrypted under it. Both
public moduli travel in the clear on the bus. RSA-512 has been factorable for
decades, so the keys protecting the session are breakable by a motivated
analyst.

**2. The key generation is badly broken — the keys share prime factors.**
This is the worse one. Across the handshakes in a single captured session, the
host's "ephemeral" RSA keys repeatedly **share prime factors** with one
another. That is the classic weak-RNG failure (as in the 2012 *Mining your Ps
and Qs* study): any two moduli that share a prime are factored **instantly** by
taking their GCD — milliseconds, no number-field sieve required. In practice a
passive observer who captures a couple of operations can recover the session
key and decrypt the password almost for free. (The drive's own RSA key is
fixed across sessions and did not fall to GCD; factoring it needs a real
RSA-512 sieve, but that too is only hours of compute.)

**3. The device's session RSA key is static.** The drive presents the same
RSA-512 public key in every handshake, so factoring it once (feasible) weakens
every session the drive will ever negotiate.

Together these mean the AES-128 data encryption — which is fine — sits behind a
session-key exchange that a USB eavesdropper can defeat. The password is not in
a capture in readable form, but it is recoverable: "insecure against analysis",
not safe.

### What this means for you

> **Unlock, change, or set your password only on a computer you trust.** On a
> machine that might be logging USB traffic, treat any password you enter as
> potentially exposed. Day to day, for a drive that spends its life locked in a
> bag, this does not change the threat model — but do not type your password on
> a kiosk or an untrusted PC.

The password-setting commands repeat a short form of this warning before they
run.

### Disclosure

These are findings about the vendor's firmware/software, not something this
tool introduces, and they are being handled as responsible security research:
the intent is to notify Kingston and allow a reasonable remediation window
before any detailed writeup. This README describes the *class* of weakness and
its threat model; it is not a turnkey tool for recovering other people's
passwords, and the findings do not help anyone access a locked drive's data.

## License

GPL-2.0-only, the same as the original project. See [COPYING](COPYING).
Based on [ironkey-unlocker](https://github.com/wltechblog/ironkey-unlocker)
by WLTechBlog.
