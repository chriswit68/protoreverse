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
    TypedLength,
};

struct FramingSpec {
    FramingKind kind = FramingKind::Auto;
    std::uint8_t width = 4;
    bool big_endian = true;
    bool includes_header = false;
    std::size_t fixed_size = 0;

    // TypedLength frames have the shape
    //   [type:type_width][length:width][payload]
    // The type field is opaque during analysis and is only interpreted when a
    // frame is encoded (for example for the server greeting).
    std::uint8_t type_width = 0;
    bool type_big_endian = true;
    std::uint64_t type_value = 0;

    bool operator==(const FramingSpec&) const = default;
};

std::string to_string(const FramingSpec& spec);

// Supports:
//   auto, raw, varint, grpc, newline, nul, fixed:<n>
//   len:<1|2|4|8>:<be|le>[:incl|:payload]
//   type:<1|2|4|8>:<be|le>+len:<1|2|4|8>:<be|le>[:incl|:payload]
//   type4+len4:le (type endianness defaults to big-endian)
//   u8, u16be, u16le, u32be, u32le, u64be, u64le
std::optional<FramingSpec> parse_framing(std::string_view text, std::string& error);

} // namespace pr
