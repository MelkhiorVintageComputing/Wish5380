// SPDX-License-Identifier: MIT
//
// Small helpers shared by every model: a byte buffer, a hex dump for failure
// messages, and a deterministic pseudo random fill so a failing test always
// reproduces from its seed alone.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wtb {

using Bytes = std::vector<uint8_t>;

std::string hex_dump(const Bytes& b, size_t max_bytes = 64);

// Deterministic pseudo random block, so a failing test always reproduces.
Bytes random_block(size_t len, uint32_t seed);

}  // namespace wtb
