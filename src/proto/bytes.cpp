// SPDX-License-Identifier: GPL-2.0-only
#include "proto/bytes.h"

#include <openssl/crypto.h>

namespace ironkey {

void secure_wipe(void* data, size_t size)
{
    if (data && size)
        OPENSSL_cleanse(data, size);
}

Bytes from_hex(std::string_view hex)
{
    if (hex.size() % 2 != 0)
        throw std::invalid_argument("odd-length hex string");
    auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::invalid_argument("bad hex digit");
    };
    Bytes out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2)
        out.push_back(static_cast<uint8_t>(nibble(hex[i]) << 4 | nibble(hex[i + 1])));
    return out;
}

std::string to_hex(ByteView data)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (uint8_t b : data) {
        out += digits[b >> 4];
        out += digits[b & 0xF];
    }
    return out;
}

}  // namespace ironkey
