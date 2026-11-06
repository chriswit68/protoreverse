#include "util/hexdump.hpp"

#include <algorithm>
#include <cstdio>

namespace pr {

std::string hexdump(std::span<const uint8_t> data, std::size_t max_bytes) {
    const std::size_t limit = max_bytes == 0 ? data.size() : max_bytes;
    const std::size_t count = std::min(data.size(), limit);
    std::string out;
    out.reserve(count * 4 + 32);

    char buffer[8];
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0) {
            out.push_back(' ');
        }
        std::snprintf(buffer, sizeof(buffer), "%02x", data[i]);
        out.append(buffer);
    }

    if (data.size() > count) {
        out.append(" ...");
    }
    return out;
}

} // namespace pr
