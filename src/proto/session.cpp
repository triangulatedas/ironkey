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

namespace {

Key128 encrypt_password(const Key128& shared, std::string_view password)
{
    if (password.size() > kMaxPasswordBytes)
        throw UnlockError("Password longer than 16 bytes. Passphrase mode (10-64 characters) "
                          "is not supported yet");
    std::array<uint8_t, 16> plaintext{};
    std::copy(password.begin(), password.end(), plaintext.begin());
    Key128 enc = aes128_ecb_encrypt(shared, plaintext);
    secure_wipe(plaintext.data(), plaintext.size());
    return enc;
}

}  // namespace

void send_change_password(Channel& ch, const Key128& shared, std::string_view old_password,
                          std::string_view new_password, Random& rng, Account account)
{
    Bytes random32 = rng.bytes(32);
    Bytes pvc_payload = encrypt_pvc_key(kPvc0Key, shared, random32);

    // Same secure-access + PVC open as unlock (observed in the capture).
    ch.command(0x8B, 0x00, Direction::None, {}, 0, {0x00, 0x03});
    ch.command(0x8F, 0x00, Direction::Write, pvc_payload, 0, {0x02});

    // FF A6 (admin) / FF A8 (user): 32 bytes = enc(old) || enc(new).
    // The device verifies the old block against the current password and
    // rejects (non-zero status) if it is wrong, so nothing changes.
    Key128 enc_old = encrypt_password(shared, old_password);
    Key128 enc_new = encrypt_password(shared, new_password);
    Bytes payload(enc_old.begin(), enc_old.end());
    payload.insert(payload.end(), enc_new.begin(), enc_new.end());

    uint8_t sub = account == Account::User ? 0xA8 : 0xA6;
    try {
        ch.command(sub, 0x00, Direction::Write, payload, 32);
    } catch (const DeviceStatusError&) {
        throw WrongPasswordError("The current password is wrong; nothing was changed");
    }
    secure_wipe(payload.data(), payload.size());

    ch.command(0x89, 0x00, Direction::None);
}

uint16_t send_configure_area(Channel& ch, const Key128& shared, uint32_t area_sectors,
                             Random& rng, bool commit)
{
    Bytes pvc_payload = encrypt_pvc_key(kPvc0Key, shared, rng.bytes(32));

    // Same secure-access + PVC open as unlock (observed in the reset capture).
    ch.command(0x8B, 0x00, Direction::None, {}, 0, {0x00, 0x03});
    ch.command(0x8F, 0x00, Direction::Write, pvc_payload, 0, {0x02});

    // FF A2 ConfigPrivateArea: payload = area size (4B LE) + a fresh random
    // area key encrypted under the session key (hypothesis: re-keying is the
    // crypto-erase). CDB bytes 6..7 = 0x41 0xA2, as captured.
    std::array<uint8_t, 16> new_area_key{};
    rng.fill(new_area_key);
    Key128 enc_key = aes128_ecb_encrypt(shared, new_area_key);
    secure_wipe(new_area_key.data(), new_area_key.size());

    Bytes payload = {static_cast<uint8_t>(area_sectors), static_cast<uint8_t>(area_sectors >> 8),
                     static_cast<uint8_t>(area_sectors >> 16),
                     static_cast<uint8_t>(area_sectors >> 24)};
    payload.insert(payload.end(), enc_key.begin(), enc_key.end());

    uint16_t a2_status = 0;
    try {
        ch.command(0xA2, 0x00, Direction::Write, payload, 20,
                   {0x00, 0x00, 0x00, 0x41, 0xA2, 0x00, 0x00, 0x00});
    } catch (const DeviceStatusError& e) {
        a2_status = e.status;
    }

    // Commit only when asked; a probe run stops here so nothing persists.
    if (commit && a2_status == 0)
        ch.command(0x89, 0x00, Direction::None);
    return a2_status;
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

void change_password(HidDevice& dev, std::string_view old_password, std::string_view new_password,
                     Account account)
{
    Channel ch(dev);
    SystemRandom rng;
    RsaKey host = RsaKey::generate512();
    Key128 shared = rsa_handshake(ch, rng, host);
    send_change_password(ch, shared, old_password, new_password, rng, account);
    secure_wipe(shared.data(), shared.size());
}

void reset_device(HidDevice& dev, std::string_view new_password, Account account)
{
    if (new_password.empty() || new_password.size() > kMaxPasswordBytes)
        throw UnlockError("New password must be 1 to 16 bytes. Passphrase mode (10-64 "
                          "characters) is not supported yet");
    Channel ch(dev);
    SystemRandom rng;

    uint32_t sectors = 0;
    {
        ch.drain();
        sectors = read_area_params(ch).data_sectors;
    }
    if (sectors == 0)
        throw IronKeyError("Could not read the data area size (FF A0)");

    // A "secure session": handshake, start secure access (type 3), open the PVC
    // with the factory PVC0 key. Returns the session key.
    auto open_session = [&]() -> Key128 {
        RsaKey host = RsaKey::generate512();
        Key128 shared = rsa_handshake(ch, rng, host);
        Bytes pvc = encrypt_pvc_key(kPvc0Key, shared, rng.bytes(32));
        ch.command(0x8B, 0x00, Direction::None, {}, 0, {0x00, 0x03});
        ch.command(0x8F, 0x00, Direction::Write, pvc, 0, {0x02});
        return shared;
    };
    // FF 26 mode flag (payload 00 02 00), in its own session (seen twice).
    auto mode_flag = [&]() {
        Key128 k = open_session();
        ch.command(0x26, 0x00, Direction::Write, Bytes{0x00, 0x02, 0x00}, 3, {0x00, 0x53});
        secure_wipe(k.data(), k.size());
    };

    // 1. Mode flag.
    mode_flag();

    // 2. Re-key the area AND set the new password, then COMMIT. This single
    //    FF A2 Configure is both the crypto-erase and the password-set: the
    //    vendor SDK (NTU_PVC_Configure_Private_Area_Key, reached from
    //    _UDV_ResetPassword) builds the token as the *password* zero-padded to
    //    16 bytes and AES-encrypted under the session key, with CDB[6]=0x41 to
    //    re-key. There is no separate "set password" command: FF A4 is only a
    //    login/verify. (Confirmed by disassembling the macOS vendor app.)
    {
        Key128 k = open_session();
        Key128 token = encrypt_password(k, new_password);
        Bytes payload = {static_cast<uint8_t>(sectors), static_cast<uint8_t>(sectors >> 8),
                         static_cast<uint8_t>(sectors >> 16), static_cast<uint8_t>(sectors >> 24)};
        payload.insert(payload.end(), token.begin(), token.end());
        ch.command(0xA2, 0x00, Direction::Write, payload, 20,
                   {0x00, static_cast<uint8_t>(account), 0x00, 0x41, 0xA2, 0x00, 0x00, 0x00});
        ch.command(0x89, 0x00, Direction::None);
        secure_wipe(k.data(), k.size());
    }

    // 3. Close the provisioning session (FF A5 = NTU_Close).
    ch.command(0xA5, 0x00, Direction::None);

    // 4. Mode flag again.
    mode_flag();

    // 5. Verify by logging in with the new password (FF A4 = NTU_Open). The
    //    vendor app does this post-set login; a wrong-password report here
    //    (status byte 6 set) would mean the FF A2 token did not take.
    {
        Key128 k = open_session();
        Key128 enc_pw = encrypt_password(k, new_password);
        ch.command(0xA4, 0x00, Direction::Write, enc_pw, 16,
                   {0x00, static_cast<uint8_t>(account), 0x00, 0x06});
        bool rejected = ch.last_status.size() > 6 && ch.last_status[6] != 0;
        ch.command(0x89, 0x00, Direction::None);
        secure_wipe(k.data(), k.size());
        if (rejected)
            throw UnlockError("Reset completed the re-key but the new password did not verify");
    }

    // NOTE: the crypto-erase leaves the data PARTITION at 0 sectors; the drive
    // will accept the new password (login succeeds) but not yet present a data
    // disk. The vendor "Format" step re-activates the partition — a domain
    // stop/start (FF 26/40/41) around a resize (MyResizeCDPartition, using
    // NTU_Calculate_Closest). That sequence is not implemented here yet, so a
    // just-reset drive is provisioned but shows no data volume until formatted.
}

void set_password(HidDevice& dev, std::string_view new_password, Account account)
{
    if (new_password.empty() || new_password.size() > kMaxPasswordBytes)
        throw UnlockError("New password must be 1 to 16 bytes. Passphrase mode (10-64 "
                          "characters) is not supported yet");
    Channel ch(dev);
    SystemRandom rng;

    // The password is set by the FF A2 Configure token (see reset_device):
    // token = enc(sessionKey, password). CDB[6]=0x41 re-keys/provisions the
    // area key under the new password. FF A4 is login only, never a setter.
    ch.drain();
    uint32_t sectors = read_area_params(ch).data_sectors;
    if (sectors == 0)
        throw IronKeyError("Could not read the data area size (FF A0)");

    RsaKey host = RsaKey::generate512();
    Key128 shared = rsa_handshake(ch, rng, host);
    Bytes pvc_payload = encrypt_pvc_key(kPvc0Key, shared, rng.bytes(32));
    ch.command(0x8B, 0x00, Direction::None, {}, 0, {0x00, 0x03});
    ch.command(0x8F, 0x00, Direction::Write, pvc_payload, 0, {0x02});

    Key128 token = encrypt_password(shared, new_password);
    Bytes payload = {static_cast<uint8_t>(sectors), static_cast<uint8_t>(sectors >> 8),
                     static_cast<uint8_t>(sectors >> 16), static_cast<uint8_t>(sectors >> 24)};
    payload.insert(payload.end(), token.begin(), token.end());
    ch.command(0xA2, 0x00, Direction::Write, payload, 20,
               {0x00, static_cast<uint8_t>(account), 0x00, 0x41, 0xA2, 0x00, 0x00, 0x00});
    ch.command(0x89, 0x00, Direction::None);
    secure_wipe(shared.data(), shared.size());
}

void write_protected_pages(HidDevice& dev, const std::vector<std::pair<uint8_t, Bytes>>& pages)
{
    Channel ch(dev);
    SystemRandom rng;

    auto open_session = [&]() -> Key128 {
        RsaKey host = RsaKey::generate512();
        Key128 shared = rsa_handshake(ch, rng, host);
        Bytes pvc = encrypt_pvc_key(kPvc0Key, shared, rng.bytes(32));
        ch.command(0x8B, 0x00, Direction::None, {}, 0, {0x00, 0x03});
        ch.command(0x8F, 0x00, Direction::Write, pvc, 0, {0x02});
        return shared;
    };

    // Mirror the capture: two pages per secure session, each session led by a
    // FF 26 mode flag. FF 62 CDB = page index, then the "rick" page key.
    for (size_t i = 0; i < pages.size(); i += 2) {
        Key128 k = open_session();
        ch.command(0x26, 0x00, Direction::Write, Bytes{0x00, 0x02, 0x00}, 3, {0x00, 0x53});
        for (size_t j = i; j < i + 2 && j < pages.size(); ++j) {
            uint8_t idx = pages[j].first;
            ch.command(0x62, 0x00, Direction::Write, pages[j].second, 512,
                       {idx, 0x00, 0x72, 0x69, 0x63, 0x6b, 0x00, 0x00});
        }
        secure_wipe(k.data(), k.size());
    }
}

}  // namespace ironkey
