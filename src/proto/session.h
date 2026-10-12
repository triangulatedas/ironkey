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

constexpr size_t kMaxPasswordBytes = 16;

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
void send_unlock(Channel& ch, const Key128& shared, std::string_view password, Random& rng);

// Complete unlock of a device already in HID mode.
void unlock(HidDevice& dev, std::string_view password);

}  // namespace ironkey
