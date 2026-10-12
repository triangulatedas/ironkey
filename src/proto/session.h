// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// A complex password is at most 16 bytes (one AES block). Passphrase mode
// accepts up to 64 characters; the device stores the first 32 and, at login,
// verifies only the first 16 (one block) — see session.cpp.
constexpr size_t kMaxPasswordBytes = 16;
constexpr size_t kMaxPassphraseBytes = 64;

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
    uint8_t admin_failed_attempts = 0; // [0x0C] consecutive failed logins on index 0
    uint8_t account_flags = 0;         // [0x1F] 0x03 single password, 0x07 admin and user
    Bytes raw;

    // Set up with separate admin and user passwords (bit 2 of 0x1F).
    bool has_user_account() const { return (account_flags & 0x04) != 0; }
};

AreaParams read_area_params(Channel& ch);

// Re-keys the data area (FF 8B, 8F, A2) with a random token — a crypto-erase
// probe that does NOT set a password (use reset_device to also set one).
// Returns the device's FF A2 status (0 = accepted). With commit=false it stops
// before the FF 89 commit, so nothing persists — a safe probe of the format.
uint16_t send_configure_area(Channel& ch, const Key128& shared, uint32_t area_sectors,
                             Random& rng, bool commit);

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

// Change a password in place (FF 8B, 8F, A6/A8, 89), as captured from the
// vendor app. Sends enc(old)||enc(new); the device rejects a wrong old
// password with no change. Account::Admin uses FF A6, Account::User FF A8.
// Throws WrongPasswordError if the current password is wrong.
void send_change_password(Channel& ch, const Key128& shared, std::string_view old_password,
                          std::string_view new_password, Random& rng,
                          Account account = Account::Admin);

// Complete unlock of a device already in HID mode.
void unlock(Channel& ch, std::string_view password, Account account = Account::Admin);

// Complete password change on a device already in HID mode.
void change_password(Channel& ch, std::string_view old_password, std::string_view new_password,
                     Account account = Account::Admin);

// Set a password with no prior password (FF A5, then 8B/8F/A4/89), as used
// after a re-key during reset/initialize. The drive must be in HID mode.
void set_password(Channel& ch, std::string_view new_password, Account account = Account::Admin);

// Owner / contact info the vendor stores in the drive's protected hidden area
// (the "IKVP" page). It is AES-ECB-encrypted under a fixed key and reachable
// through the factory PVC0 session, so it can be read and written WITHOUT the
// drive password. The password hints therefore travel with the drive and are
// readable pre-unlock — see PROTOCOL.md.
struct Profile {
    std::string admin_hint;
    std::string user_hint;
    std::string name;
    std::string company;
    std::string description;

    bool empty() const
    {
        return admin_hint.empty() && user_hint.empty() && name.empty() && company.empty() &&
               description.empty();
    }
};

// Reads the owner profile (PVC0 session, FF 63 page 0x0D, decrypt). Returns an
// empty Profile if the page has no IKVP header. No password needed; HID mode.
Profile read_profile(Channel& ch);

// Writes the owner profile (PVC0 session, FF 62 pages 0x0D/0x0E). Overwrites
// all fields. No password needed; HID mode.
void write_profile(Channel& ch, const Profile& profile);

// Replays FF 62 "write protected page" commands (the vendor Format step that
// activates the data partition after a reset). Each entry is (page_index,
// 512-byte payload); the payloads are session-independent, so captured bytes
// replay verbatim. Pages are written two per secure session with a FF 26 mode
// flag, mirroring the capture. The drive must be in HID mode.
void write_protected_pages(Channel& ch,
                           const std::vector<std::pair<uint8_t, Bytes>>& pages);

// Full factory reset / initialize (the vendor "Reset Device" flow): mode flag,
// then a single committed FF A2 Configure that both re-keys the area
// (crypto-erase, DESTROYS ALL DATA) and sets `new_password` (the token is the
// password encrypted under the session key, CDB[6]=0x41 to re-key), finally a
// login to verify. The drive must be in HID mode; no old password is needed.
// Afterwards create a filesystem on the (blank) data partition with mkfs.
void reset_device(Channel& ch, std::string_view new_password, Account account = Account::Admin);

}  // namespace ironkey
