// SPDX-License-Identifier: GPL-2.0-only
// End-to-end test against a simulated IronKey that runs the device side of
// the protocol with real keys, so the RSA and AES maths are checked from both
// ends rather than against recorded bytes only.
#include <cstring>
#include <deque>

#include "check.h"
#include "proto/session.h"

using namespace ironkey;

namespace {

class SimulatedIronKey : public HidDevice {
public:
    explicit SimulatedIronKey(std::string password) : password_(std::move(password)) {}

    bool unlocked = false;
    bool host_proved_key = false;

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

        if (reading_)
            response_ = handle_read();
        else if (len_ == 0)
            inputs_.push_back(status_report(handle_write({})));
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
        inputs_.push_back(status_report(handle_write(data)));
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
    Bytes status_report(uint16_t status)
    {
        Bytes r(64, 0);
        r[0] = 1;
        r[2] = sub_;
        r[4] = status & 0xFF;
        r[5] = status >> 8;
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
            return host_proved_key ? 0 : 0x0010;
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
            pvc_open_ = secure_access_ && std::equal(b0.begin(), b0.end(), data.begin()) &&
                        std::equal(b1.begin(), b1.end(), data.begin() + 16);
            return pvc_open_ ? 0 : 0x0020;
        }
        case 0xA4: {
            std::array<uint8_t, 16> expected{};
            std::memcpy(expected.data(), password_.data(), password_.size());
            auto enc = aes128_ecb_encrypt(shared_, expected);
            password_ok_ = std::equal(enc.begin(), enc.end(), data.begin());
            return password_ok_ ? 0 : 0x0030;
        }
        case 0x89:
            unlocked = host_proved_key && pvc_open_ && password_ok_;
            return unlocked ? 0 : 0x0040;
        }
        throw check::Failure{"unexpected write command " + std::to_string(sub_)};
    }

    std::string password_;
    SystemRandom rng_;
    RsaKey key_ = RsaKey::generate512();
    Bytes host_n_, host_e_, r1_, r2_, nonce_, response_;
    Key128 shared_{};
    bool secure_access_ = false, pvc_open_ = false, password_ok_ = false;

    uint8_t sub_ = 0;
    uint32_t len_ = 0;
    bool reading_ = false;
    Cdb cdb_{};
    std::deque<Bytes> inputs_;
};

void run_unlock(SimulatedIronKey& dev, std::string_view password)
{
    Channel ch(dev);
    ch.read_delay = std::chrono::milliseconds(0);
    SystemRandom rng;
    RsaKey host = RsaKey::generate512();
    Key128 shared = rsa_handshake(ch, rng, host);
    send_unlock(ch, shared, password, rng);
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
        CHECK(dev.unlocked);
    }
}

TEST(wrong_password_reports_device_status)
{
    SimulatedIronKey dev("right");
    try {
        run_unlock(dev, "wrong");
        throw check::Failure{"expected DeviceStatusError"};
    } catch (const DeviceStatusError& e) {
        CHECK_EQ(e.command, 0xA4);
    }
    CHECK(!dev.unlocked);
}
