#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace pr {

// Returns space-separated lowercase hex bytes. max_bytes == 0 means unlimited;
// truncated output ends with " ...".
std::string hexdump(std::span<const uint8_t> data, std::size_t max_bytes = 64);

} // namespace pr
