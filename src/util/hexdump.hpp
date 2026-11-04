#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace pr {

std::string hexdump(std::span<const uint8_t> data, std::size_t max_bytes = 64);

} // namespace pr
