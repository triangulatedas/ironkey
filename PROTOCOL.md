# IronKey Locker+ Authentication Protocol

This document describes the complete unlock sequence for the Kingston
IronKey Locker+ 50G2, as reverse-engineered from the macOS IronKey
application binary and verified on actual hardware.

## Device States

The IronKey has two USB personalities:

| State | PID | Interfaces | Description |
|-------|-----|------------|-------------|
| Locked | varies by model | Mass Storage (CD-ROM + 0B disk) | Read-only setup partition |
| HID mode | varies by model | Mass Storage + HID | Accepts authentication commands |

The PID varies by device capacity (e.g., 0x159D for 50G2). The tool
discovers PIDs at runtime via the USB product string.

## Step 1: PID Switch (activateHIDInterface)

When locked, the device only exposes mass storage. To reach the HID
interface, a magic sequence of USB string descriptor reads triggers a
firmware-mode PID switch.

**Prerequisites**: Unmount all volumes (CD-ROM LUN 1 and data LUN 0).

**Sequence**: Read USB string descriptors via standard GET_DESCRIPTOR
control transfers:

```
bmRequestType = 0x80  (device-to-host, standard)
bRequest      = 0x06  (GET_DESCRIPTOR)
wValue        = 0x03XX (STRING, XX = descriptor index)
wIndex        = 0x0409 (US English)
wLength       = 16
```

**Pattern** (6 reads, ~100ms apart):

```
Read string descriptor at iSerialNumber + 2
Read string descriptor at iSerialNumber + 2
Read string descriptor at iSerialNumber + 3
Read string descriptor at iSerialNumber + 3
Read string descriptor at iSerialNumber + 2
Read string descriptor at iSerialNumber + 2
```

After the last read, the device disconnects and re-enumerates with a new
PID that includes a HID interface (interface 1, HID boot-level).

## Step 2: HID Transport

All authentication commands use HID reports via the hidraw device:

| Operation | Method |
|-----------|--------|
| Send command | `write()` — Output report (64 bytes + report ID 0x00) |
| Send data | `ioctl(HIDIOCSFEATURE)` — Feature report (512 bytes) |
| Read data | `ioctl(HIDIOCGFEATURE)` — Feature report (512 bytes) |
| Status | `read()` — Interrupt endpoint (64 bytes) |

### Output Report Format (64 bytes)

```
Offset  Size  Field
------  ----  -----
0x00    1     Subcommand (0x81, 0x82, ..., 0xA4, etc.)
0x01    1     CDB[2] — secondary parameter
0x02    4     Data length (little-endian uint32)
0x06    4     Chunk count (1 if data, 0 if no data)
0x0A    4     Magic: 0x4D524F54 ("MROT")
0x0E    1     Direction: 0=write, 1=read
0x0F    1     Reserved (0)
0x10    8     CDB[3:10] — subcommand parameters
0x18    1     CDB[11] — final parameter
0x19    7     Padding (zeros)
```

### Interrupt Response Format (64 bytes)

Each command produces two interrupt input reports: an acknowledgement after
the command header, and a status report once the command (and any feature
transfer) has completed. Observed on a Locker+50G2:

```
Acknowledgement:  00 01 00 00 00 00 00 00 ...
Status report:    00 00 <cmd> 00 <status lo> <status hi> <result> 00 ...

Offset  Size  Field
------  ----  -----
0x02    1     Command echo (e.g., 0xA4)
0x04    2     Status (little-endian): 0x0000 = command processed
0x06    1     Command-specific result. For FF A4: 0x00 = password
              accepted, 0x01 = wrong password
```

**Note:** a wrong password does *not* produce a non-zero status. Every
command, including FF A4 and FF 89, returns status 0; only byte 0x06 of the
FF A4 status report tells the outcome. (FF 89's acknowledgement is not
always sent; the status report can arrive first.)

## Step 3: RSA-512 Key Exchange (FF 81-88)

A mutual RSA-512 challenge-response establishes a 16-byte shared secret.
Both the host and device generate ephemeral RSA-512 key pairs.

### FF 81 — Get Device Public Key (READ 128 bytes)

```
Output report:  subcommand=0x81, direction=read, data_length=128
Response:       N_dev (64 bytes, little-endian) || E_dev (64 bytes, little-endian)
```

The device returns its RSA modulus (N) and public exponent (E). Both are
512-bit values in little-endian byte order.

### FF 82 — Send Host Public Key (WRITE 128 bytes)

```
Output report:  subcommand=0x82, direction=write, data_length=128
Feature data:   N_host (64 bytes) || E_host (64 bytes)
```

Host sends its own RSA-512 public key.

### FF 83 — Send Host Challenge (WRITE 64 bytes)

```
Output report:  subcommand=0x83, direction=write, data_length=64
Feature data:   r1 (64 bytes) — random challenge, clamped < N_dev
```

The host generates a random 64-byte value `r1`, clamped so it is
numerically less than `N_dev`. `r1` is sent in the clear.

### FF 84 — Get Device Signature (READ 64 bytes)

```
Output report:  subcommand=0x84, direction=read, data_length=64
Response:       sig1 = Sign_device(r1)
```

The device signs `r1` with its private key, proving possession of the
corresponding private key. (The signature is not verified by the host in
practice — this step establishes the challenge is fresh.)

### FF 85 — Get Device Nonce (READ 64 bytes)

```
Output report:  subcommand=0x85, direction=read, data_length=64
Response:       nonce_dev (64 bytes) — device-generated random nonce
```

### FF 86 — Send Encrypted Nonce (WRITE 64 bytes)

```
Output report:  subcommand=0x86, direction=write, data_length=64
Feature data:   nonce_dev ^ D_host mod N_host (as 64 bytes LE)
```

The host encrypts the device's nonce with its own private key, proving it
received the nonce and holds the corresponding private key.

### FF 87 — Send Encrypted Challenge (WRITE 64 bytes)

```
Output report:  subcommand=0x87, direction=write, data_length=64
Feature data:   r2 ^ E_dev mod N_dev (as 64 bytes LE)
```

The host generates a fresh random `r2` (clamped < N_dev) and encrypts it
with the device's public key. The device will decrypt this with its
private key.

### FF 88 — Get Device Secret (READ 64 bytes)

```
Output report:  subcommand=0x88, direction=read, data_length=64
Response:       r2_decoded ^ D_host mod N_host
```

The device decrypts `r2` from FF 87, then encrypts it with the host's
public key. The host decrypts with its private key to recover `r2_dev`.

### Shared Secret Derivation

```
r2_dev = pow(response_from_FF88, D_host, N_host)
shared_secret = r2[:16] XOR r2_dev[:16]
```

The first 16 bytes of the host's `r2` and the device's decrypted `r2_dev`
are XORed to produce a 16-byte AES key used for all subsequent operations.

## Step 4: PVC Key Exchange (FF 8B, FF 8F)

### FF 8B — Start Secure Access

```
Output report:  subcommand=0x8B, CDB[3]=0x00, CDB[4]=0x03
```

Opens a secure session with `secureType=3` (PVC domain).

### FF 8F — PVCFuncOpen (WRITE 64 bytes)

```
Output report:  subcommand=0x8F, CDB[3]=0x02
Feature data:   encryptPVCKey(PVC0_KEY, shared_secret) — 64 bytes
```

Sends a device-specific PVC key encrypted under the session key. The PVC
key is hardcoded per device and derived from the device's provisioning
data.

### encryptPVCKey Algorithm

```
hash1 = SHA-256(PVC0_KEY)
local[0:32]  = hash1
local[32:48] = shared_secret
hash2 = SHA-256(local[0:48])
local[0:32]  = hash2
local[32:64] = random(32 bytes)
result = AES-128-ECB(shared_secret, local[0:16])
       + AES-128-ECB(shared_secret, local[16:32])
       + AES-128-ECB(shared_secret, local[32:48])
       + AES-128-ECB(shared_secret, local[48:64])
```

Four AES-128-ECB blocks encrypted with the shared secret, totaling 64
bytes.

## Step 5: Password Authentication (FF A4, FF 89)

### FF A4 — NTU_Open (WRITE 16 bytes)

```
Output report:  subcommand=0xA4, CDB[3]=0x00, CDB[4]=0x00, CDB[5]=0x00, CDB[6]=0x06
Feature data:   AES-128-ECB(shared_secret, password_block) — 16 bytes
```

`password_block` is the 16-byte value the device actually verifies, then
AES-128-ECB encrypted with the shared secret. Byte 0x06 of the status report
for this command is 1 if the password was wrong (see "Interrupt Response
Format").

#### Deriving `password_block` (vendor-compatible)

Reverse-engineered from the vendor application's `DriveSDK::hashPassword`
(`IronKey.app`, a Qt app; confirmed on both the x86_64 and arm64 slices) and
verified on a Locker+50G2:

- **Length ≤ 16** — the UTF-8 password bytes, zero-padded to 16. The device
  compares this directly; every byte counts, and there is nothing past byte 16.
- **Length 17–64 (passphrase)** — `MD5(utf8(password))` (a 16-byte digest). The
  vendor uses exactly this cutoff (`length >= 17`) and Qt's `QCryptographicHash`
  algorithm `1` (MD5). This is why a long passphrase needs the *whole* string:
  the full text feeds the MD5, and its first/only 16 bytes become the block.

The set-token (`FF A2`) uses the same `password_block`, so provisioning and
login always agree, and both interoperate with the vendor app.

Hardware-verified examples:
- `"Abcd1234#$!!geir"` (16 chars) → `41626364313233342324212167656972`
- `"Dette er en innmari lang text"` (29 chars) →
  `md5` = `26003f01ddc64bc36b9526317bfc1971`

### Provisioning one or two accounts (`FF A2` payload length)

The `FF A2` ConfigPrivateArea payload is `area_size[4 LE]` followed by **one
16-byte token per account**, each `AES-128-ECB(session_key, password_block)`.
The device infers the account count from the payload length, `(len - 4) / 16`:

| Payload | Accounts | Layout |
|---|---|---|
| 20 bytes | admin only | `size[4]` + admin token |
| 36 bytes | admin + user | `size[4]` + admin token + user token |

The admin token comes first (userIDIndex 0), the user token second (index 1),
matching `FF A4`'s account selector. **The CDB is identical in both cases**
(`FF A2 00 00 00 00 41 A2 …`, CDB[1] = 0); only the length changes. Confirmed
against the vendor captures (`ironkey-fileds`, `ironkey-passphrase` show the
36-byte form) and hardware-verified: after `reset --admin-user`, the drive
reports `account_flags = 0x07` (admin+user) and both accounts unlock
independently.

The vendor app also has a separate `MyAes`/`CRijndael` transform with a fixed
`"PDMG…"` key (`encryptPasssword`); that is for *local* password handling, **not**
the device token — a red herring during reverse-engineering.

CDB parameters:
- CDB[3] = secureType (0x00)
- CDB[4] = userIDIndex (0x00)
- CDB[5] = timeInterval (0x00)
- CDB[6] = autoLogout (0x06)

### FF 89 — Commit

```
Output report:  subcommand=0x89, no data
```

Finalizes the authentication. If the encrypted password was correct, the
device exposes the encrypted data partition. This command was the last
piece discovered and was critical to making unlock work.

## Step 6: Verification

After FF 89, the device re-enumerates the mass storage LUN 0 to expose
the encrypted data partition (e.g., /dev/sda). The tool polls `lsblk` for
up to 10 seconds to detect the new partition.

## Complete Sequence Summary

```
FF 81  READ  128B   Get device RSA public key (N, E)
FF 82  WRITE 128B   Send host RSA public key (N, E)
FF 83  WRITE 64B    Send host challenge r1
FF 84  READ  64B    Get device signature on r1
FF 85  READ  64B    Get device nonce
FF 86  WRITE 64B    Send encrypted nonce (proves host identity)
FF 87  WRITE 64B    Send encrypted r2 (challenge for device)
FF 88  READ  64B    Get device's decryption of r2

                      Derive shared_secret = r2[:16] XOR r2_dev[:16]

FF 8B  NONE          Start Secure Access (secureType=3)
FF 8F  WRITE 64B    PVCFuncOpen (encrypted PVC key)
FF A4  WRITE 16B    NTU_Open (encrypted user password)
FF 89  NONE          Commit (finalize authentication)

                      Device exposes data partition
```

## Status queries (FF 21, FF A0)

Both are plain reads (direction read, 0x200 bytes) and work **without a
session or password**, before or after the RSA handshake. Neither uses up a
login attempt: a handshake that is never followed by FF A4 does not change
the failed-attempt counter, even if power is then removed. Observed on a
Locker+50G2 (64 GB), set up with both admin and user passwords.

### FF 21 — GetConfig

```
02 02 00 [02] 00 | 00 40 2F 07 | 03 01 [0A] 00 00 | B0 04
```

| Offset | Value | Meaning |
|---|---|---|
| 0x03 | `02` locked, `00` unlocked | lock state (probable) |
| 0x05–0x08 | `0x072F4000` LE | data area size in 512-byte sectors (61.7 GB) |
| 0x0B | `0A` before any session since power-on, `08` once a session has been started | session flag (bit 1); not a counter, resets on replug |
| 0x0E–0x0F | `0x04B0` = 1200 | unknown; possibly an auto-lock timeout in seconds |

### FF A0 — GetAreaParams

| Offset | Value | Meaning |
|---|---|---|
| 0x00–0x03, 0x04–0x07 | `0x072F4000` LE | data area size in sectors (twice) |
| 0x08 | `00` locked, `01` unlocked | data area open |
| **0x0C** | **0, 1, 2, …** | **consecutive failed logins**; increments on each wrong password, resets to 0 on a successful login |
| 0x1E–0x1F | `01 07` | unknown, constant so far |
| 0x20 | `00` locked, `06` unlocked | mirrors the autoLogout parameter sent with FF A4 (CDB[6] = 0x06) |
| 0x21–0x23 | changes between sessions (`00 00 03`, `00 00 02`, `00 00 18`, `28 00 2E`, …) | unknown session/state value; **not** a counter |

Kingston's manual gives a default limit (MaxNoA) of **10 consecutive failed
logins per account**. Reaching it on the User account locks User login (the
Admin can reset it); reaching it on the **Admin** account crypto-erases the
drive. Where the limit itself is stored has not been identified.

#### Detecting a blank (never-initialized) drive

A factory-fresh, never-initialized Locker+50G2 reads (captured from a real blank
unit, both before and after a handshake):

- `GetAreaParams[0x0C] = 1` — where a freshly provisioned drive reads `0`.
- `account_flags (0x1F) = 0x03` — identical to a single-password drive.
- `data_sectors = 0x072F4000` — the full physical size, so size is **not** a
  signal; a blank drive still reports its capacity.
- `GetConfig` is identical to a provisioned drive apart from the session flag.

So the **only** distinguishing byte is `0x0C`, which is also the admin
failed-login counter. A blank drive is therefore indistinguishable from a
provisioned drive that has had **exactly one** failed login, using only the
no-session queries. Treat `0x0C == 1` (with `account_flags != 0x07`) as a *hint*
that the drive may be uninitialized — enough to offer setup rather than nag for a
password — but never as grounds to auto-erase. The counter resets to 0 on any
successful unlock, so the ambiguity clears itself. (`read_device_summary()` sets
`maybe_uninitialized` on this basis; `ironkey-unlock status` reports it.)

### Accounts (userIDIndex)

FF A4's CDB[4] selects the account. With admin and user passwords both set:

- **Index 0 is the Admin account.** The admin password unlocks with index 0.
  The user password with index 0 is rejected and counted as a failed login
  in 0x0C.
- **Index 1 is the User account.** The user password unlocks with index 1
  (CDB `00 01 00 06`).
- GetAreaParams looks the same in an admin and a user session (apart from the
  changing 0x21–0x23 value), so it does not show which account logged in.
- FF A4 status byte 0x06 flags a wrong password for both accounts.
- Each account has its own failed-login counter, as Kingston's manual says: a
  failed user login leaves GetAreaParams 0x0C (admin) unchanged. The **user
  counter is not exposed by FF 21 or FF A0**. A failed user login changed
  only GetConfig 0x0B (session flag) and GetAreaParams 0x23 (varies every
  session). The vendor app probably reads it with a command not yet known.

The original single-password set-up (as used by the Python tool) also logs
in with index 0.

## Subcommand Reference

| Subcmd | Name | Dir | Size | Description |
|--------|------|-----|------|-------------|
| 0x21 | GetConfig | READ | 0x200 | Device configuration |
| 0x41 | CD_Unlock | NONE | — | CD-ROM unlock (alternative path) |
| 0x81 | GetDevPubKey | READ | 128 | Device RSA public key |
| 0x82 | SetHostPubKey | WRITE | 128 | Host RSA public key |
| 0x83 | HostChallenge | WRITE | 64 | Host random challenge r1 |
| 0x84 | DevSignature | READ | 64 | Device signature of r1 |
| 0x85 | GetDevNonce | READ | 64 | Device random nonce |
| 0x86 | EncryptedNonce | WRITE | 64 | Host-encrypted device nonce |
| 0x87 | EncryptedChallenge | WRITE | 64 | Encrypted r2 for device |
| 0x88 | DevSecret | READ | 64 | Device-decrypted r2 |
| 0x89 | Commit | NONE | — | Finalize session |
| 0x8B | StartSecureAccess | NONE | — | Open secure domain |
| 0x8F | PVCFuncOpen | WRITE | 64 | PVC key exchange |
| 0xA0 | GetAreaParams | READ | 0x200 | Area parameters / status, incl. failed-login count (see above) |
| 0xA2 | ConfigPrivateArea | WRITE | — | Configure data area |
| 0xA4 | NTU_Open | WRITE | 16 | Encrypted password auth |

## Management command set (reverse-engineered; not yet implemented)

Beyond unlock, the vendor application exposes a full management API. These
notes come from static analysis of the Windows and macOS vendor binaries
(for interoperability). They are **not implemented yet** and the exact
payloads still need confirming on hardware before use.

### Transport

Every vendor command is a 10-byte SCSI CDB `FF <subfunc> <params...>`, sent
via SCSI pass-through (Windows `DeviceIoControl`, Linux `SG_IO`). Many are
also reachable over the HID transport the unlock path uses. Note: the Linux
kernel blocks vendor opcode `0xFF` for unprivileged `SG_IO`, so SCSI-path
features need `CAP_SYS_RAWIO` (a `setcap` helper), unlike the HID unlock path
which needs no privilege.

### Command subfunctions (second CDB byte)

| Range | Purpose |
|---|---|
| `04` (04 00/02/03…09) | Device info (descriptors, serial, MaxNoA). `FF 04 01` returns a 512-byte info block. |
| `21` | GetConfig (512-byte read) |
| `80`–`88` | RSA handshake (same as the HID `81`–`88`) |
| `89` | Commit |
| `8B` | Start secure access |
| `8F` | PVCFuncOpen (param 1/2/3) |
| `A0` | GetAreaParams (512-byte read; byte `0x0C` = admin failed-login count) |
| `A2` | ConfigPrivateArea (write) — used when (re)partitioning the data area |
| `A4` | NTU_Open — password verify (CDB[2]=userIDIndex: 0 admin, 1 user) |
| `A5`–`AA`, `E2` | further area/account operations |
| `60`–`6E` | account/profile block |

### SDK operations (from macOS symbols) and their primitives

| Operation | SDK method | Device primitive |
|---|---|---|
| Initialize / set passwords | `DriveSDK::setPassword(admin, user, mode)` | `UDV_SetExternalPVC0`, `encryptPVCKey` |
| Change password | `DriveSDK::changePassword(old, new, flag)` | — |
| Reset user password (as admin) | `DriveSDK::resetUserPassword(new)` | — |
| Recovery password | `DriveSDK::createRecoveryPassword`, `useRecoveryPasswordToCreateNewUserPassword` | — |
| **Reset / crypto-erase** | `DriveSDK::formatPartition()` | **`UDV_FormatPartition(lun, buf, len, 0)`** |
| Retry counter | `DriveSDK::getPasswordRemainRetryCount(account)` | `FF 04 01` device-info read (`remaining = max − failed`) |
| Read-only / hidden area | `DriveSDK::setupProtectedHiddenArea`, `read/writeProtectHiddenArea` | `UDV_Read/WriteProtectHidden_Type1` |

### Accounts

At most two password accounts — admin (index 0) and user (index 1) — plus an
optional one-time recovery password. `MaxNoA` (max consecutive failed logins)
defaults to 10 per account; the device reports it in the `FF 04 01` block.
Reaching it crypto-erases on the admin account, or locks out on the user
account (admin can then reset).

### Reset and Initialize (architecture)

Reverse engineering the macOS `DriveSDK` shows **Reset Device and Initialize
are the same core operation**. The forgotten-password "Reset Device" prompts
for a *new* password and re-keys the drive; it does not need the old password.
Both reduce to:

1. **Set password + re-key** — `DriveSDK::setPassword(admin, user, mode)`:
   - `CheckActiveFunc` → `GetPVC0Status` → `DevGetICType`
   - `UDV_SetPassword` (dedicated set command — *not* the `FF A4` verify) which
     installs a new **external PVC0 key** via `UDV_SetExternalPVC0`. Replacing
     the key cryptographically abandons the old data (this is the crypto-erase).
   - `UDV_SetPWS_ProtectHidden`, `UDV_Open/CloseContinuousAccess`,
     `UDV_GetStatus`, wrapped by `UDV_Login`/`UDV_Logout`.
2. **Rebuild filesystem** — `DriveSDK::formatPartition()` → `UDV_FormatPartition`
   → `Def_buildTable_Hid` + `buildFAT32/16/12`, which writes a new partition
   table and FAT over the HID transport (subcommands `0xF0`/`0xF1`/`0xF2`;
   no elevated privilege needed).

Still to pin down before implementing: the exact `UDV_SetPassword` payload
(encrypted new password + PVC key material) and the `0xF0/F1/F2` report
layouts. These are built through Objective-C `SCSITask`/IOKit dispatch on
macOS; the Windows binary builds the CDBs directly and is the better source
for the exact bytes. Testing is safe to iterate on an already-blank drive
(initialize), but each attempt re-keys, so treat it as destructive.

### Change password (captured, implemented)

Captured from the vendor "change password" and "change user password" flows.
Unlike reset, it does **not** re-key the area (no `FF A2`), so it is fully
implemented. Sequence:

```
handshake (FF 80..88) -> FF 8B 00 00 03 -> FF 8F 02 (PVC) -> FF A6/A8 (32B) -> FF 89
```

| CDB | Meaning |
|---|---|
| `FF A6` + 32B | Change admin password: `AES-ECB(sessionKey, oldPw) \|\| AES-ECB(sessionKey, newPw)` (each zero-padded to 16B, same as the unlock password block). |
| `FF A8` + 32B | Change the user password (admin sets it), same payload shape. |

The device verifies the first block against the current password and rejects
the command with a non-zero status (no change) if it is wrong. A following
session writes the password hint via `FF 62` (optional; not reproduced).

### Reset / initialize sequence (captured)

Confirmed by a USB capture of the vendor app performing "Reset Device" with a
new password (Bulk-Only Transport; each vendor command is a CBW carrying the
`FF xx` CDB, an optional data phase, then a CSW). The crypto pieces (RSA
handshake, `encryptPVCKey` for `FF 8F`, AES-ECB password for `FF A4`) are the
same ones the unlock path already implements. New commands:

| CDB | Meaning |
|---|---|
| `FF A2 00 00 00 00 41 A2` + 20B | ConfigPrivateArea: payload = area size in sectors (4B LE) + 16B token. Defines/re-keys the encrypted area (the crypto-erase). |
| `FF A4 … 06` + 16B | Set password (same framing as the unlock *verify*; sets because the area was just (re)configured). 16B = AES-ECB(sessionKey, password padded). |
| `FF 89` | Commit (follows A2, and follows A4). |
| `FF A5` | Enter password-provisioning state (sent before the set-password session). |
| `FF 07 01 <lba 4B> <len>` | Prepare/erase a data region (no data phase); used before writing the fresh filesystem. |
| `FF 62 / FF 63 <idx> …` + 512B | Read/write an encrypted profile field (hint / contact). The 512B payload is encrypted; the CDB carries a field index and a fixed 4-byte constant (`72 69 63 6b`, not user data — it is identical across captures regardless of the password set). |
| `FF 26 00 00 53` + 3B (`00 02 00`) | Set a mode flag during provisioning. |
| WRITE10 (`2A`) | Standard SCSI writes that lay down the new partition table + FAT (the `buildFAT*` output). |

Overall flow: configure/erase area (`handshake → 8B(type3) → 8F 02 → A2 → 89`)
→ provisioning (`A5`, profile writes `62/63`, `26`) → set password
(`handshake → 8B(type3) → 8F 02 → A4 → 89`) → write the FAT filesystem
(`FF 07` region prep + `WRITE10`). Reset and initialize are the same flow.
