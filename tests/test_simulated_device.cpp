// SPDX-License-Identifier: GPL-2.0-only
// End-to-end test against a simulated IronKey that runs the device side of
// the protocol with real keys, so the RSA and AES maths are checked from both
// ends rather than against recorded bytes only.
//
// Like the real device (observed on a Locker+50G2), the simulation answers
// every command with status OK, even for a wrong password. A wrong password is
// flagged in byte 6 of the FF A4 status report, and counted in GetAreaParams
// byte 0x0C until the next successful login.
#include <cstring>
#include <deque>

#include "check.h"
#include "proto/session.h"

using namespace ironkey;

namespace {

class SimulatedIronKey : public HidDevice {
public:
    // `user_password` empty = single-password drive (only index 0 exists).
    explicit SimulatedIronKey(std::string admin_password, std::string user_password = "")
        : passwords_{std::move(admin_password), std::move(user_password)}
    {
    }

    bool unlocked = false;
    bool host_proved_key = false;
    bool pvc_open = false;
    bool password_ok = false;
    // Per-account counters, as the manual describes. Only the admin counter's
    // location in GetAreaParams (0x0C) is known.
    uint8_t failed_attempts[2] = {0, 0};
    uint8_t last_account = 0xFF;

    void write_output(ByteView report) override
    {
        CHECK_EQ(report.size(), kOutputReportSize);
        CHECK_EQ(report[0x0A] | report[0x0B] << 8 | report[0x0C] << 16 | report[0x0D] << 24,
                 0x4D524F54);
        sub_ = report[0];
        len_ = report[2] | report[3] << 8 | report[4] << 16 | report[5] << 24;
        reading_ = report[0x0E] == 1;
        std::memcpy(cdb_.data(), &report[0x10], cdb_.size());
        inputs_.push_back(status_report(0));  // acknowledgement

        result_ = 0;
        if (reading_)
            response_ = handle_read();
        else if (len_ == 0)
            inputs_.push_back(status_report(handle_write({}), result_));
    }

    std::optional<Bytes> read_input(std::chrono::milliseconds) override
    {
        if (inputs_.empty())
            return std::nullopt;
        Bytes r = inputs_.front();
        inputs_.pop_front();
        return r;
    }

    void set_feature(ByteView data) override
    {
        CHECK(!reading_);
        CHECK_EQ(data.size(), static_cast<size_t>(len_));
        uint16_t status = handle_write(data);
        inputs_.push_back(status_report(status, result_));
    }

    Bytes get_feature() override
    {
        CHECK(reading_);
        Bytes r = response_;
        r.resize(kFeatureReportSize);
        inputs_.push_back(status_report(0));
        return r;
    }

private:
    Bytes status_report(uint16_t status, uint8_t result = 0)
    {
        Bytes r(64, 0);
        r[0] = 1;
        r[2] = sub_;
        r[4] = status & 0xFF;
        r[5] = status >> 8;
        r[6] = result;
        return r;
    }

    Bytes handle_read()
    {
        switch (sub_) {
        case 0x81: {
            Bytes pub = key_.n;
            pub.insert(pub.end(), key_.e.begin(), key_.e.end());
            return pub;
        }
        case 0x84:
            return mod_exp_le(r1_, key_.d, key_.n);
        case 0x85:
            nonce_ = rng_.bytes(63);
            nonce_.push_back(0);
            return nonce_;
        case 0xA0: {
            Bytes r(0x200, 0);
            r[0] = 0x00, r[1] = 0x40, r[2] = 0x2F, r[3] = 0x07;
            r[0x08] = unlocked ? 1 : 0;
            r[0x0C] = failed_attempts[0];
            return r;
        }
        case 0x88: {
            Bytes r_dev = rng_.bytes(63);
            r_dev.push_back(0);
            for (size_t i = 0; i < 16; ++i)
                shared_[i] = r2_[i] ^ r_dev[i];
            return mod_exp_le(r_dev, host_e_, host_n_);
        }
        }
        throw check::Failure{"unexpected read command " + std::to_string(sub_)};
    }

    uint16_t handle_write(ByteView data)
    {
        switch (sub_) {
        case 0x82:
            host_n_.assign(data.begin(), data.begin() + 64);
            host_e_.assign(data.begin() + 64, data.end());
            return 0;
        case 0x83:
            r1_.assign(data.begin(), data.end());
            return 0;
        case 0x86:
            host_proved_key = mod_exp_le(data, host_e_, host_n_) == nonce_;
            return 0;
        case 0x87:
            r2_ = mod_exp_le(data, key_.d, key_.n);
            return 0;
        case 0x8B:
            secure_access_ = cdb_[1] == 0x03;
            return 0;
        case 0x8F: {
            // First two AES blocks must be the expected hash2.
            std::array<uint8_t, 48> local{};
            auto h1 = sha256(kPvc0Key);
            std::copy(h1.begin(), h1.end(), local.begin());
            std::copy(shared_.begin(), shared_.end(), local.begin() + 32);
            auto h2 = sha256(local);
            auto b0 = aes128_ecb_encrypt(shared_, ByteView(h2.data(), 16));
            auto b1 = aes128_ecb_encrypt(shared_, ByteView(h2.data() + 16, 16));
            pvc_open = secure_access_ && std::equal(b0.begin(), b0.end(), data.begin()) &&
                       std::equal(b1.begin(), b1.end(), data.begin() + 16);
            return 0;
        }
        case 0xA4: {
            last_account = cdb_[1];
            const std::string* pw = last_account < 2 ? &passwords_[last_account] : nullptr;
            password_ok = false;
            if (pw && !pw->empty()) {
                std::array<uint8_t, 16> expected{};
                std::memcpy(expected.data(), pw->data(), pw->size());
                auto enc = aes128_ecb_encrypt(shared_, expected);
                password_ok = std::equal(enc.begin(), enc.end(), data.begin());
            }
            if (!password_ok) {
                if (last_account < 2)
                    ++failed_attempts[last_account];
                result_ = 1;
            }
            return 0;
        }
        case 0x89:
            unlocked = host_proved_key && pvc_open && password_ok;
            if (unlocked)
                failed_attempts[last_account] = 0;
            return 0;
        }
        throw check::Failure{"unexpected write command " + std::to_string(sub_)};
    }

    std::string passwords_[2];
    SystemRandom rng_;
    RsaKey key_ = RsaKey::generate512();
    Bytes host_n_, host_e_, r1_, r2_, nonce_, response_;
    Key128 shared_{};
    bool secure_access_ = false;

    uint8_t sub_ = 0;
    uint8_t result_ = 0;
    uint32_t len_ = 0;
    bool reading_ = false;
    Cdb cdb_{};
    std::deque<Bytes> inputs_;
};

void run_unlock(SimulatedIronKey& dev, std::string_view password,
                Account account = Account::Admin)
{
    Channel ch(dev);
    ch.read_delay = std::chrono::milliseconds(0);
    SystemRandom rng;
    RsaKey host = RsaKey::generate512();
    Key128 shared = rsa_handshake(ch, rng, host);
    send_unlock(ch, shared, password, rng, account);
}

}  // namespace

TEST(generated_rsa512_key_is_valid)
{
    RsaKey k = RsaKey::generate512();
    CHECK_EQ(k.n.size(), 64u);
    CHECK(k.n[63] & 0x80);  // exactly 512 bits
    Bytes m = from_hex("0102030405060708090a0b0c0d0e0f10");
    m.resize(64);
    CHECK(mod_exp_le(mod_exp_le(m, k.e, k.n), k.d, k.n) == m);
}

TEST(unlocks_simulated_device_with_correct_password)
{
    for (int i = 0; i < 5; ++i) {
        SimulatedIronKey dev("hunter2-hunter2!");
        run_unlock(dev, "hunter2-hunter2!");
        CHECK(dev.host_proved_key);
        CHECK(dev.pvc_open);
        CHECK(dev.password_ok);
        CHECK(dev.unlocked);
    }
}

TEST(wrong_password_is_reported_and_counted)
{
    SimulatedIronKey dev("right");
    CHECK_THROWS_AS(run_unlock(dev, "wrong"), WrongPasswordError);
    CHECK(dev.host_proved_key);
    CHECK(dev.pvc_open);
    CHECK(!dev.unlocked);
    CHECK_THROWS_AS(run_unlock(dev, "wrong again"), WrongPasswordError);

    Channel ch(dev);
    ch.read_delay = std::chrono::milliseconds(0);
    AreaParams p = read_area_params(ch);
    CHECK_EQ(int(p.admin_failed_attempts), 2);
    CHECK(!p.unlocked);

    run_unlock(dev, "right");
    CHECK(dev.unlocked);
    p = read_area_params(ch);
    CHECK_EQ(int(p.admin_failed_attempts), 0);
    CHECK(p.unlocked);
}

TEST(user_and_admin_accounts_are_separate)
{
    SimulatedIronKey dev("admin-pw", "user-pw");

    // The user password is wrong for the admin account (index 0), as observed
    // on hardware, and counts against the admin.
    CHECK_THROWS_AS(run_unlock(dev, "user-pw", Account::Admin), WrongPasswordError);
    CHECK_EQ(int(dev.failed_attempts[0]), 1);

    // A failed user login counts against the user only.
    CHECK_THROWS_AS(run_unlock(dev, "admin-pw", Account::User), WrongPasswordError);
    CHECK_EQ(int(dev.failed_attempts[1]), 1);
    CHECK_EQ(int(dev.failed_attempts[0]), 1);

    // A successful user login resets only the user counter.
    run_unlock(dev, "user-pw", Account::User);
    CHECK(dev.unlocked);
    CHECK_EQ(int(dev.last_account), 1);
    CHECK_EQ(int(dev.failed_attempts[1]), 0);
    CHECK_EQ(int(dev.failed_attempts[0]), 1);
}

TEST(single_password_drive_has_no_user_account)
{
    SimulatedIronKey dev("only-pw");
    CHECK_THROWS_AS(run_unlock(dev, "only-pw", Account::User), WrongPasswordError);
    run_unlock(dev, "only-pw", Account::Admin);
    CHECK(dev.unlocked);
}

// Replies captured from a real Locker+50G2 (see PROTOCOL.md, "Status queries").
TEST(decodes_real_get_area_params_replies)
{
    struct Case {
        const char* hex;
        bool unlocked;
        int failed;
        bool has_user;
    };
    const Case cases[] = {
        // admin+user drive, locked, after two wrong admin passwords
        {"00402f0700402f0700000000020000000000000000000000000000000000010700000043", false, 2,
         true},
        // admin+user drive, unlocked with the admin password
        {"00402f0700402f07010000000000000000000000000000000000000000000107064d004f", true, 0,
         true},
        // single-password drive, locked
        {"00402f0700402f0700000000000000000000000000000000000000000000010300000001", false, 0,
         false},
    };
    for (const auto& c : cases) {
        class Fixed : public HidDevice {
        public:
            explicit Fixed(Bytes r) : reply(std::move(r)) { reply.resize(kFeatureReportSize); }
            Bytes reply;
            int inputs = 0;
            void write_output(ByteView) override { inputs = 2; }
            std::optional<Bytes> read_input(std::chrono::milliseconds) override
            {
                if (inputs == 0)
                    return std::nullopt;
                --inputs;
                return Bytes(64, 0);
            }
            void set_feature(ByteView) override {}
            Bytes get_feature() override { return reply; }
        } dev(from_hex(c.hex));
        Channel ch(dev);
        ch.read_delay = std::chrono::milliseconds(0);
        AreaParams p = read_area_params(ch);
        CHECK_EQ(p.data_sectors, 0x072F4000u);  // 61.7 GB
        CHECK_EQ(p.unlocked, c.unlocked);
        CHECK_EQ(int(p.admin_failed_attempts), c.failed);
        CHECK_EQ(p.has_user_account(), c.has_user);
    }
}
