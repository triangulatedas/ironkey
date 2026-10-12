// SPDX-License-Identifier: GPL-2.0-only
// Compares the C++ protocol against vectors produced by the original Python
// implementation (tools/gen_vectors.py).
#include <deque>

#include "check.h"
#include "proto/session.h"

using namespace ironkey;

namespace {

struct ClampCase {
    const char* buf;
    const char* n_dev;
    const char* expected;
};

struct PvcCase {
    const char* shared;
    const char* random32;
    const char* expected;
};

struct FrameCase {
    uint8_t sub;
    uint8_t cdb2;
    Direction dir;
    const char* data;
    uint32_t data_len;
    const char* cdb;
    const char* report;
};

enum class WriteKind { Output, Feature };

struct TranscriptWrite {
    WriteKind kind;
    const char* hex;
};

#include "vectors.inc"

std::string hex(ByteView b) { return to_hex(b); }

Key128 key128(const char* h)
{
    Bytes b = from_hex(h);
    Key128 k{};
    std::copy(b.begin(), b.end(), k.begin());
    return k;
}

Cdb cdb_from(const char* h)
{
    Bytes b = from_hex(h);
    Cdb c{};
    std::copy(b.begin(), b.end(), c.begin());
    return c;
}

// Plays back canned feature reports and records everything written.
class ReplayDevice : public HidDevice {
public:
    std::deque<Bytes> feature_responses;
    std::vector<std::pair<WriteKind, std::string>> writes;

    void write_output(ByteView report) override
    {
        writes.emplace_back(WriteKind::Output, hex(report));
        pending_ += 1;  // acknowledgement
        pending_ += 1;  // status
    }

    std::optional<Bytes> read_input(std::chrono::milliseconds) override
    {
        if (pending_ == 0)
            return std::nullopt;
        --pending_;
        return Bytes(64, 0);
    }

    void set_feature(ByteView data) override { writes.emplace_back(WriteKind::Feature, hex(data)); }

    Bytes get_feature() override
    {
        Bytes r = feature_responses.front();
        feature_responses.pop_front();
        r.resize(kFeatureReportSize);
        return r;
    }

private:
    int pending_ = 0;
};

// Hands out a fixed byte stream.
class StreamRandom : public Random {
public:
    explicit StreamRandom(Bytes stream) : stream_(std::move(stream)) {}
    void fill(std::span<uint8_t> out) override
    {
        if (pos_ + out.size() > stream_.size())
            throw std::runtime_error("StreamRandom exhausted");
        std::copy_n(stream_.begin() + pos_, out.size(), out.begin());
        pos_ += out.size();
    }
    size_t remaining() const { return stream_.size() - pos_; }

private:
    Bytes stream_;
    size_t pos_ = 0;
};

}  // namespace

TEST(clamp_matches_python)
{
    for (const auto& c : kClampCases)
        CHECK_EQ(hex(clamp_to_modulus(from_hex(c.buf), from_hex(c.n_dev))), std::string(c.expected));
}

TEST(encrypt_pvc_key_matches_python)
{
    for (const auto& c : kPvcCases)
        CHECK_EQ(hex(encrypt_pvc_key(kPvc0Key, key128(c.shared), from_hex(c.random32))),
                 std::string(c.expected));
}

TEST(command_framing_matches_python)
{
    for (const auto& c : kFrameCases) {
        auto report = build_command_report(c.sub, c.cdb2, c.dir,
                                           c.data_len ? c.data_len : from_hex(c.data).size(),
                                           cdb_from(c.cdb));
        CHECK_EQ(hex(report), std::string(c.report));
    }
}

TEST(full_unlock_transcript_matches_python)
{
    namespace t = transcript;

    ReplayDevice dev;
    for (const char* r : t::kFeatureResponses)
        dev.feature_responses.push_back(from_hex(r));

    Channel ch(dev);
    ch.read_delay = std::chrono::milliseconds(0);
    StreamRandom rng(from_hex(t::kRandomStream));
    RsaKey host(from_hex(t::kHostN), from_hex(t::kHostE), from_hex(t::kHostD));

    Key128 shared = rsa_handshake(ch, rng, host);
    CHECK_EQ(hex(shared), std::string(t::kSharedSecret));

    send_unlock(ch, shared, t::kPassword, rng);
    CHECK_EQ(rng.remaining(), 0u);
    CHECK(dev.feature_responses.empty());

    CHECK_EQ(dev.writes.size(), std::size(t::kWrites));
    for (size_t i = 0; i < dev.writes.size(); ++i) {
        CHECK(dev.writes[i].first == t::kWrites[i].kind);
        CHECK_EQ(dev.writes[i].second, std::string(t::kWrites[i].hex));
    }
}

TEST(password_too_long_is_rejected_before_sending)
{
    ReplayDevice dev;
    Channel ch(dev);
    StreamRandom rng(Bytes(32));
    // 65 characters exceeds the passphrase limit (64).
    std::string too_long(65, 'x');
    CHECK_THROWS_AS(send_unlock(ch, Key128{}, too_long, rng), UnlockError);
    CHECK(dev.writes.empty());
}
