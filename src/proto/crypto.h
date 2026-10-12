// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "proto/bytes.h"

namespace ironkey {

class CryptoError : public IronKeyError {
public:
    using IronKeyError::IronKeyError;
};

// Source of random bytes. Tests replace it with a fixed stream.
class Random {
public:
    virtual ~Random() = default;
    virtual void fill(std::span<uint8_t> out) = 0;

    Bytes bytes(size_t n)
    {
        Bytes b(n);
        fill(b);
        return b;
    }
};

// Cryptographically secure random bytes from OpenSSL.
class SystemRandom : public Random {
public:
    void fill(std::span<uint8_t> out) override;
};

// RSA-512 key. All three values are 64-byte little-endian integers,
// the byte order the device uses on the wire.
struct RsaKey {
    Bytes n;
    Bytes e;
    Bytes d;

    RsaKey() = default;
    RsaKey(Bytes n_, Bytes e_, Bytes d_) : n(std::move(n_)), e(std::move(e_)), d(std::move(d_)) {}
    RsaKey(const RsaKey&) = default;
    RsaKey& operator=(const RsaKey&) = default;
    ~RsaKey() { secure_wipe(d.data(), d.size()); }

    static RsaKey generate512();
};

// (base ^ exp) mod mod, all little-endian. Result is zero-padded to out_len.
Bytes mod_exp_le(ByteView base, ByteView exp, ByteView mod, size_t out_len = 64);

Key128 aes128_ecb_encrypt(const Key128& key, ByteView block);
std::array<uint8_t, 32> sha256(ByteView data);

}  // namespace ironkey
