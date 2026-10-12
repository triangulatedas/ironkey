# ironkey

Unlock Kingston IronKey Locker+ G2 encrypted USB drives on Linux, without
root and without Python.

Kingston only ships unlock software for Windows and macOS. This project is a
C++ rewrite of [ironkey-unlocker](https://github.com/wltechblog/ironkey-unlocker),
the Python tool that first made these drives usable on Linux.

> This does not let anyone unlock a drive without its password, and it does
> not exploit any weakness in the device. It only gives Linux users the same
> legitimate access that the vendor software gives on other platforms.

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

The protocol itself is unchanged: the same commands, parameters and crypto,
byte for byte.

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

| Phase | State |
|---|---|
| 1. Protocol port, tested against the Python implementation | done |
| 2. Hardware check of the udev `uaccess` rule (no root) | done: mode switch and handshake work as a normal user |
| 3. Multi-device CLI | done: `list`, `unlock`, `hid-mode`, `handshake` |
| 4. Session agent (udev monitor, password prompt, notifications) | done; hotplug verified in `--dry-run`; real unlock via agent untested |
| 5. Debian package | todo |
| 6. CI builds for amd64 / arm64 | todo |

Still not possible on Linux, as in the original: first-time setup of a new
drive, and passphrase mode. Set the drive up with the vendor software first.

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
ironkey-unlock lock [--device PORT]        # unmount and power off, which locks the drive
ironkey-unlock hid-mode  [--device PORT]   # switch a locked drive to HID mode
ironkey-unlock handshake [--device PORT]   # diagnostic: key exchange only, no password sent
```

`--device PORT` is needed only when more than one IronKey is connected.

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

## Security note: the handshake uses RSA-512

This tool implements the drive's own authentication protocol faithfully,
including a weakness worth understanding.

Every unlock — and any operation that sets a password (initialize, reset,
change password) — negotiates a session key under **ephemeral RSA-512 keys**,
then sends your password AES-encrypted under that key. RSA-512 has been
factorable for decades (hours on a workstation, less on cloud hardware), and
both public moduli travel in the clear on the USB bus. So anyone who can
**passively record the USB traffic while you unlock or set a password** can,
with modest effort, factor the keys, recover the session key, and decrypt your
password. The password is not in the capture in readable form, but it is
recoverable by someone motivated — "insecure against analysis", not
"plaintext".

This is not introduced here; it is how the device works, and this tool cannot
change it. The drive's real threat model is a lost or stolen *locked* drive,
where there is no handshake to capture, so day to day this does not matter.
But the implication is simple:

> **Unlock, reset, initialize, or change your password only on a computer you
> trust.** If you are especially cautious, do password-setting operations on a
> clean or throwaway system where no USB-capture tool could be running. On a
> machine you do not control, treat any password you enter as potentially
> exposed.

The password-setting commands (reset / initialize / change) repeat a short
form of this warning before they run.

## License

GPL-2.0-only, the same as the original project. See [COPYING](COPYING).
Based on [ironkey-unlocker](https://github.com/wltechblog/ironkey-unlocker)
by WLTechBlog.
