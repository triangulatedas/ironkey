// SPDX-License-Identifier: GPL-2.0-only
#include "proto/crypto.h"

#include <memory>

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace ironkey {

namespace {

struct BnFree {
    void operator()(BIGNUM* bn) const { BN_clear_free(bn); }
};
struct BnCtxFree {
    void operator()(BN_CTX* ctx) const { BN_CTX_free(ctx); }
};
struct CipherCtxFree {
    void operator()(EVP_CIPHER_CTX* ctx) const { EVP_CIPHER_CTX_free(ctx); }
};

using Bn = std::unique_ptr<BIGNUM, BnFree>;
using BnCtx = std::unique_ptr<BN_CTX, BnCtxFree>;
using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, CipherCtxFree>;

[[noreturn]] void fail(const char* what)
{
    unsigned long err = ERR_get_error();
    std::string msg = std::string("OpenSSL: ") + what;
    if (err != 0) {
        char buf[256];
        ERR_error_string_n(err, buf, sizeof buf);
        msg += ": ";
        msg += buf;
    }
    throw CryptoError(msg);
}

Bn new_bn()
{
    Bn bn(BN_new());
    if (!bn)
        fail("BN_new");
    return bn;
}

BnCtx new_ctx()
{
    BnCtx ctx(BN_CTX_new());
    if (!ctx)
        fail("BN_CTX_new");
    return ctx;
}

Bn bn_from_le(ByteView data)
{
    Bn bn(BN_lebin2bn(data.data(), static_cast<int>(data.size()), nullptr));
    if (!bn)
        fail("BN_lebin2bn");
    return bn;
}

Bytes bn_to_le(const BIGNUM* bn, size_t len)
{
    Bytes out(len);
    if (BN_bn2lebinpad(bn, out.data(), static_cast<int>(len)) < 0)
        throw CryptoError("number does not fit in " + std::to_string(len) + " bytes");
    return out;
}

}  // namespace

void SystemRandom::fill(std::span<uint8_t> out)
{
    if (RAND_bytes(out.data(), static_cast<int>(out.size())) != 1)
        fail("RAND_bytes");
}

RsaKey RsaKey::generate512()
{
    BnCtx ctx = new_ctx();
    Bn e = new_bn();
    BN_set_word(e.get(), 65537);

    for (;;) {
        Bn p = new_bn(), q = new_bn(), n = new_bn();
        if (!BN_generate_prime_ex(p.get(), 256, 0, nullptr, nullptr, nullptr) ||
            !BN_generate_prime_ex(q.get(), 256, 0, nullptr, nullptr, nullptr))
            fail("BN_generate_prime_ex");
        if (!BN_mul(n.get(), p.get(), q.get(), ctx.get()))
            fail("BN_mul");
        if (BN_num_bits(n.get()) != 512)
            continue;

        Bn p1 = new_bn(), q1 = new_bn(), phi = new_bn();
        if (!BN_sub(p1.get(), p.get(), BN_value_one()) ||
            !BN_sub(q1.get(), q.get(), BN_value_one()) ||
            !BN_mul(phi.get(), p1.get(), q1.get(), ctx.get()))
            fail("BN arithmetic");

        Bn d(BN_mod_inverse(nullptr, e.get(), phi.get(), ctx.get()));
        if (!d) {
            // e and phi not coprime; try new primes.
            ERR_clear_error();
            continue;
        }
        return RsaKey(bn_to_le(n.get(), 64), bn_to_le(e.get(), 64), bn_to_le(d.get(), 64));
    }
}

Bytes mod_exp_le(ByteView base, ByteView exp, ByteView mod, size_t out_len)
{
    BnCtx ctx = new_ctx();
    Bn b = bn_from_le(base), e = bn_from_le(exp), m = bn_from_le(mod);
    Bn result = new_bn();
    if (!BN_mod_exp(result.get(), b.get(), e.get(), m.get(), ctx.get()))
        fail("BN_mod_exp");
    return bn_to_le(result.get(), out_len);
}

Key128 aes128_ecb_encrypt(const Key128& key, ByteView block)
{
    if (block.size() != 16)
        throw CryptoError("AES block must be 16 bytes");

    CipherCtx ctx(EVP_CIPHER_CTX_new());
    if (!ctx)
        fail("EVP_CIPHER_CTX_new");
    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_128_ecb(), nullptr, key.data(), nullptr) != 1)
        fail("EVP_EncryptInit_ex");
    EVP_CIPHER_CTX_set_padding(ctx.get(), 0);

    Key128 out{};
    int len = 0;
    if (EVP_EncryptUpdate(ctx.get(), out.data(), &len, block.data(), 16) != 1 || len != 16)
        fail("EVP_EncryptUpdate");
    return out;
}

Bytes aes128_ecb_decrypt(const Key128& key, ByteView data)
{
    if (data.size() % 16 != 0)
        throw CryptoError("AES-ECB input must be a multiple of 16 bytes");

    CipherCtx ctx(EVP_CIPHER_CTX_new());
    if (!ctx)
        fail("EVP_CIPHER_CTX_new");
    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_128_ecb(), nullptr, key.data(), nullptr) != 1)
        fail("EVP_DecryptInit_ex");
    EVP_CIPHER_CTX_set_padding(ctx.get(), 0);

    Bytes out(data.size());
    int len = 0;
    if (EVP_DecryptUpdate(ctx.get(), out.data(), &len, data.data(),
                          static_cast<int>(data.size())) != 1)
        fail("EVP_DecryptUpdate");
    out.resize(static_cast<size_t>(len));
    return out;
}

std::array<uint8_t, 32> sha256(ByteView data)
{
    std::array<uint8_t, 32> out{};
    unsigned int len = 0;
    if (EVP_Digest(data.data(), data.size(), out.data(), &len, EVP_sha256(), nullptr) != 1)
        fail("EVP_Digest");
    return out;
}

std::array<uint8_t, 16> md5(ByteView data)
{
    std::array<uint8_t, 16> out{};
    unsigned int len = 0;
    if (EVP_Digest(data.data(), data.size(), out.data(), &len, EVP_md5(), nullptr) != 1)
        fail("EVP_Digest(md5)");
    return out;
}

}  // namespace ironkey
