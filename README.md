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
| Run by hand with `sudo python3 ironkey_unlock.py` | A session agent notices the drive being plugged in and asks for the password *(planned)* |
| Assumes exactly one IronKey is connected | Handles several IronKeys at once, each tracked separately |
| Needs Python 3 and `pycryptodome` | Compiled C++20 binary, depends only on OpenSSL and libudev |
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
  it exposes the password in `ps` and shell history. *(The unlock command is
  planned to read from a prompt or from stdin.)*
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

On real hardware (Locker+50G2), the mode switch and the RSA handshake have
been verified running as a normal user. The password and commit steps have
not yet been run on hardware.

## Status

| Phase | State |
|---|---|
| 1. Protocol port, tested against the Python implementation | done |
| 2. Hardware check of the udev `uaccess` rule (no root) | done: mode switch and handshake work as a normal user |
| 3. Multi-device CLI | in progress: `list`, `hid-mode`, `handshake`; `unlock` next |
| 4. Session agent (udev monitor, password prompt, notifications) | todo |
| 5. Debian package | todo |
| 6. CI builds for amd64 / arm64 | todo |

Still not possible on Linux, as in the original: first-time setup of a new
drive, and passphrase mode. Set the drive up with the vendor software first.

## Building

Requires CMake ≥ 3.20, a C++20 compiler, OpenSSL 3 and libudev:

```bash
sudo apt install build-essential cmake libssl-dev libudev-dev
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Usage

Install the udev rule once, then replug the drive:

```bash
sudo cp packaging/70-ironkey.rules /etc/udev/rules.d/
sudo udevadm control --reload
```

```bash
ironkey-unlock list                        # connected Kingston devices, state, nodes and access
ironkey-unlock hid-mode  [--device PORT]   # switch a locked drive to HID mode
ironkey-unlock handshake [--device PORT]   # diagnostic: key exchange only, no password sent
```

Example:

```
$ ironkey-unlock list
port 2-1  0951:159d  Locker+50G2  S/N 80C5F260C660BA70F1F32591  [locked]
    usb     /dev/bus/usb/002/007 (rw)
    cdrom   /dev/sr0 (0.1 GiB)
    disk    /dev/sda (empty)
```

> IronKey drives erase themselves after too many wrong passwords. Neither the
> CLI nor the planned agent ever retries a password automatically.

## Hardware notes

Tested with a Locker+50G2: locked PID `0951:159d`, HID mode PID `0951:00d3`.
The drive keeps its USB port path and serial across the switch. Other
capacities (16G2, 32G2, …) use other PIDs. Devices are recognised by vendor ID
and product name, so they are expected to work, but have not been tested.

## Layout

```
src/proto/   Protocol: command framing, RSA handshake, unlock. Depends only on OpenSSL.
src/linux/   Linux device access: hidraw, usbfs mode switch, sysfs topology, volumes.
src/cli/     ironkey-unlock command.
packaging/   udev rule.
tests/       Unit tests; vectors.inc is generated from the original Python code.
tools/       gen_vectors.py
```

To regenerate the test vectors (needs `python3-cryptography`, not
`pycryptodome`):

```bash
python3 tools/gen_vectors.py /path/to/ironkey-unlocker/ironkey_unlock.py > tests/vectors.inc
```

## License

GPL-2.0-only, the same as the original project. See [COPYING](COPYING).
Based on [ironkey-unlocker](https://github.com/wltechblog/ironkey-unlocker)
by WLTechBlog.
