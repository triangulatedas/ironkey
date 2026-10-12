// SPDX-License-Identifier: GPL-2.0-only
#include "proto/session.h"

#include <algorithm>

namespace ironkey {

const std::array<uint8_t, 64> kPvc0Key = {
    0x6a, 0xed, 0x67, 0xb9, 0x8b, 0xcf, 0x83, 0xf2, 0xa0, 0xd6, 0x22, 0x7f, 0xb8, 0xa3, 0x58, 0xc4,
    0x74, 0xe7, 0xdb, 0x47, 0x2b, 0x9d, 0xdc, 0xd5, 0x09, 0xaf, 0x8b, 0xef, 0x1c, 0xb9, 0xc8, 0xb8,
    0x94, 0x9c, 0xe6, 0x41, 0xcc, 0xaf, 0xe1, 0xd5, 0x84, 0x7d, 0x8d, 0xcc, 0x79, 0xe1, 0xba, 0x18,
    0x21, 0x5c, 0x80, 0xb7, 0x5c, 0x4e, 0x10, 0xdd, 0x64, 0x42, 0xff, 0xac, 0xbc, 0xbd, 0x16, 0x84,
};

namespace {

Bytes read_exact(Channel& ch, uint8_t sub, uint32_t len, const char* what)
{
    Bytes resp = ch.command(sub, 0x00, Direction::Read, {}, len);
    if (resp.size() < len)
        throw HandshakeError(std::string("Failed to read ") + what);
    return resp;
}

}  // namespace

Bytes clamp_to_modulus(ByteView buf, ByteView n_dev)
{
    Bytes b(buf.begin(), buf.end());
    uint8_t top = n_dev[63] > 0 ? n_dev[63] - 1 : 0;
    for (size_t i = b.size(); i-- > 0;) {
        if (b[i] != 0) {
            b[i] = top;
            break;
        }
    }
    return b;
}

Bytes encrypt_pvc_key(ByteView pvc0_key, const Key128& shared, ByteView random32)
{
    std::array<uint8_t, 64> local{};

    auto hash1 = sha256(pvc0_key);
    std::copy(hash1.begin(), hash1.end(), local.begin());
    std::copy(shared.begin(), shared.end(), local.begin() + 32);

    auto hash2 = sha256(ByteView(local.data(), 48));
    std::copy(hash2.begin(), hash2.end(), local.begin());
    std::copy(random32.begin(), random32.begin() + 32, local.begin() + 32);

    Bytes result;
    result.reserve(64);
    for (size_t i = 0; i < 4; ++i) {
        Key128 block = aes128_ecb_encrypt(shared, ByteView(local.data() + i * 16, 16));
        result.insert(result.end(), block.begin(), block.end());
    }
    secure_wipe(local.data(), local.size());
    return result;
}

Key128 rsa_handshake(Channel& ch, Random& rng, const RsaKey& host)
{
    ch.drain();

    // FF 81: device public key
    Bytes dev_pub = read_exact(ch, 0x81, 128, "device public key (FF 81)");
    ByteView n_dev(dev_pub.data(), 64);
    ByteView e_dev(dev_pub.data() + 64, 64);

    // FF 82: host public key
    Bytes host_pub = host.n;
    host_pub.insert(host_pub.end(), host.e.begin(), host.e.end());
    ch.command(0x82, 0x00, Direction::Write, host_pub);

    // FF 83: host challenge r1, sent in the clear
    Bytes r1 = clamp_to_modulus(rng.bytes(64), n_dev);
    ch.command(0x83, 0x00, Direction::Write, r1);

    // FF 84: device signature over r1 (not verified, as in the vendor app)
    read_exact(ch, 0x84, 64, "device signature (FF 84)");

    // FF 85 / 86: prove possession of the host private key
    Bytes nonce_dev = read_exact(ch, 0x85, 64, "device nonce (FF 85)");
    ch.command(0x86, 0x00, Direction::Write, mod_exp_le(nonce_dev, host.d, host.n));

    // FF 87: r2 encrypted to the device
    Bytes r2 = clamp_to_modulus(rng.bytes(64), n_dev);
    ch.command(0x87, 0x00, Direction::Write, mod_exp_le(r2, e_dev, n_dev));

    // FF 88: device secret, encrypted to the host
    Bytes sig2 = read_exact(ch, 0x88, 64, "device secret (FF 88)");
    Bytes r_dev = mod_exp_le(sig2, host.d, host.n);

    Key128 shared{};
    for (size_t i = 0; i < shared.size(); ++i)
        shared[i] = r2[i] ^ r_dev[i];

    secure_wipe(r2.data(), r2.size());
    secure_wipe(r_dev.data(), r_dev.size());
    return shared;
}

const char* to_string(Account account)
{
    return account == Account::User ? "user" : "admin";
}

void send_unlock(Channel& ch, const Key128& shared, std::string_view password, Random& rng,
                 Account account)
{
    if (password.size() > kMaxPasswordBytes)
        throw UnlockError("Password longer than 16 bytes. Passphrase mode (10-64 characters) "
                          "is not supported yet");

    Bytes random32 = rng.bytes(32);
    Bytes pvc_payload = encrypt_pvc_key(kPvc0Key, shared, random32);

    // FF 8B: start secure access, secureType=3
    ch.command(0x8B, 0x00, Direction::None, {}, 0, {0x00, 0x03});

    // FF 8F: PVCFuncOpen
    ch.command(0x8F, 0x00, Direction::Write, pvc_payload, 0, {0x02});

    // FF A4: NTU_Open with the zero-padded, encrypted password.
    // CDB: secureType=0, userIDIndex, timeInterval=0, autoLogout=6.
    std::array<uint8_t, 16> plaintext{};
    std::copy(password.begin(), password.end(), plaintext.begin());
    Key128 encrypted_pw = aes128_ecb_encrypt(shared, plaintext);
    secure_wipe(plaintext.data(), plaintext.size());
    ch.command(0xA4, 0x00, Direction::Write, encrypted_pw, 16,
               {0x00, static_cast<uint8_t>(account), 0x00, 0x06});
    // Status stays 0 for a wrong password; byte 6 of the report is set instead.
    bool rejected = ch.last_status.size() > 6 && ch.last_status[6] != 0;

    // FF 89: commit. Sent either way, matching the sequence verified on
    // hardware for both outcomes.
    ch.command(0x89, 0x00, Direction::None);

    if (rejected)
        throw WrongPasswordError("Wrong password");
}

AreaParams read_area_params(Channel& ch)
{
    AreaParams p;
    p.raw = ch.command(0xA0, 0x00, Direction::Read, {}, 0x200);
    if (p.raw.size() < 0x10)
        throw IronKeyError("Short GetAreaParams reply");
    p.data_sectors = p.raw[0] | p.raw[1] << 8 | p.raw[2] << 16 | uint32_t(p.raw[3]) << 24;
    p.unlocked = p.raw[0x08] != 0;
    p.admin_failed_attempts = p.raw[0x0C];
    p.account_flags = p.raw[0x1F];
    return p;
}

void unlock(HidDevice& dev, std::string_view password, Account account)
{
    Channel ch(dev);
    SystemRandom rng;
    RsaKey host = RsaKey::generate512();
    Key128 shared = rsa_handshake(ch, rng, host);
    send_unlock(ch, shared, password, rng, account);
    secure_wipe(shared.data(), shared.size());
}

}  // namespace ironkey
