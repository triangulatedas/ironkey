// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <string_view>

#include "proto/channel.h"
#include "proto/crypto.h"

namespace ironkey {

class HandshakeError : public IronKeyError {
public:
    using IronKeyError::IronKeyError;
};

class UnlockError : public IronKeyError {
public:
    using IronKeyError::IronKeyError;
};

// The device rejected the password (FF A4 status report, byte 6).
class WrongPasswordError : public UnlockError {
public:
    using UnlockError::UnlockError;
};

constexpr size_t kMaxPasswordBytes = 16;

// Which password to log in with: FF A4 CDB[4] (userIDIndex).
enum class Account : uint8_t {
    // The admin password on drives with admin and user passwords, or the only
    // password on single-password drives. Ten failures crypto-erase the drive.
    Admin = 0,
    // The user password on drives with admin and user passwords. Ten failures
    // lock user login; the admin can reset it.
    User = 1,
};

const char* to_string(Account account);

// The out-of-box limit of consecutive failed logins per account (MaxNoA)
// according to Kingston's manual. Reaching it on the admin account
// crypto-erases the drive.
constexpr int kDefaultMaxFailedAttempts = 10;

// Decoded GetAreaParams (FF A0). Works without a session or password.
// See PROTOCOL.md, "Status queries".
struct AreaParams {
    uint32_t data_sectors = 0;         // [0x00] size of the encrypted data area
    bool unlocked = false;             // [0x08]
    uint8_t admin_failed_attempts = 0; // [0x0C] consecutive failed admin (index 0) logins
    Bytes raw;
};

AreaParams read_area_params(Channel& ch);

// Fixed PVC key from the vendor application (see PROTOCOL.md, Step 4).
extern const std::array<uint8_t, 64> kPvc0Key;

// Lowers the most significant non-zero byte of `buf` so the value is below
// the device modulus. Mirrors the vendor application.
Bytes clamp_to_modulus(ByteView buf, ByteView n_dev);

// encryptPVCKey from the vendor application. `random32` is the 32 bytes of
// padding that end up in the last two AES blocks.
Bytes encrypt_pvc_key(ByteView pvc0_key, const Key128& shared, ByteView random32);

// RSA-512 key exchange (FF 81..88). Returns the 16-byte session key.
Key128 rsa_handshake(Channel& ch, Random& rng, const RsaKey& host_key);

// PVC open, password and commit (FF 8B, 8F, A4, 89).
// Throws WrongPasswordError if the device rejects the password.
void send_unlock(Channel& ch, const Key128& shared, std::string_view password, Random& rng,
                 Account account = Account::Admin);

// Complete unlock of a device already in HID mode.
void unlock(HidDevice& dev, std::string_view password, Account account = Account::Admin);

}  // namespace ironkey
