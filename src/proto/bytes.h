// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ironkey {

using Bytes = std::vector<uint8_t>;
using ByteView = std::span<const uint8_t>;
using Key128 = std::array<uint8_t, 16>;

class IronKeyError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

Bytes from_hex(std::string_view hex);
std::string to_hex(ByteView data);

// Overwrites memory in a way the compiler will not optimise away.
void secure_wipe(void* data, size_t size);

}  // namespace ironkey
