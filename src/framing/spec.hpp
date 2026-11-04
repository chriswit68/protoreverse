#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pr {

enum class FramingKind {
    Auto,
    Raw,
    Length,
    Varint,
    Grpc,
    Newline,
    Nul,
    Fixed,
};

struct FramingSpec {
    FramingKind kind = FramingKind::Auto;
    std::uint8_t width = 4;
    bool big_endian = true;
    bool includes_header = false;
    std::size_t fixed_size = 0;

    bool operator==(const FramingSpec&) const = default;
};

std::string to_string(const FramingSpec& spec);

// Supports:
//   auto, raw, varint, grpc, newline, nul, fixed:<n>
//   len:<1|2|4|8>:<be|le>[:incl|:payload]
//   u8, u16be, u16le, u32be, u32le, u64be, u64le
std::optional<FramingSpec> parse_framing(std::string_view text, std::string& error);

} // namespace pr
