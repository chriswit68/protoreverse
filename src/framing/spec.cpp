#include "framing/spec.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <vector>

namespace pr {
namespace {

std::string lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::vector<std::string> split(std::string_view text, char delimiter) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t pos = text.find(delimiter, start);
        if (pos == std::string_view::npos) {
            parts.emplace_back(text.substr(start));
            break;
        }
        parts.emplace_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

std::optional<std::size_t> parse_size(std::string_view text) {
    std::size_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

} // namespace

std::string to_string(const FramingSpec& spec) {
    switch (spec.kind) {
    case FramingKind::Auto:
        return "auto";
    case FramingKind::Raw:
        return "raw";
    case FramingKind::Varint:
        return "varint";
    case FramingKind::Grpc:
        return "grpc";
    case FramingKind::Newline:
        return "newline";
    case FramingKind::Nul:
        return "nul";
    case FramingKind::Fixed:
        return "fixed:" + std::to_string(spec.fixed_size);
    case FramingKind::Length: {
        std::string out = "len:" + std::to_string(spec.width) + ":" +
                          (spec.big_endian ? "be" : "le");
        if (spec.includes_header) {
            out += ":incl";
        }
        return out;
    }
    case FramingKind::TypedLength: {
        std::string out = "type:" + std::to_string(spec.type_width) + ":" +
                          (spec.type_big_endian ? "be" : "le") + "+len:" +
                          std::to_string(spec.width) + ":" +
                          (spec.big_endian ? "be" : "le");
        if (spec.includes_header) {
            out += ":incl";
        }
        return out;
    }
    }
    return "auto";
}

std::optional<FramingSpec> parse_framing(std::string_view text, std::string& error) {
    const std::string value = lower(text);

    FramingSpec spec;
    if (value == "auto") {
        spec.kind = FramingKind::Auto;
        return spec;
    }
    if (value == "raw") {
        spec.kind = FramingKind::Raw;
        return spec;
    }
    if (value == "varint" || value == "delimited") {
        spec.kind = FramingKind::Varint;
        return spec;
    }
    if (value == "grpc") {
        spec.kind = FramingKind::Grpc;
        return spec;
    }
    if (value == "newline" || value == "line") {
        spec.kind = FramingKind::Newline;
        return spec;
    }
    if (value == "nul" || value == "null") {
        spec.kind = FramingKind::Nul;
        return spec;
    }

    const auto parts = split(value, ':');
    if (!parts.empty() && parts[0] == "fixed") {
        if (parts.size() != 2) {
            error = "fixed framing requires fixed:<size>";
            return std::nullopt;
        }
        const auto size = parse_size(parts[1]);
        if (!size || *size == 0) {
            error = "invalid fixed frame size";
            return std::nullopt;
        }
        spec.kind = FramingKind::Fixed;
        spec.fixed_size = *size;
        return spec;
    }

    if (!parts.empty() && parts[0] == "len") {
        if (parts.size() < 3 || parts.size() > 4) {
            error = "length framing requires len:<1|2|4|8>:<be|le>[:incl|:payload]";
            return std::nullopt;
        }
        const auto width = parse_size(parts[1]);
        if (!width || (*width != 1 && *width != 2 && *width != 4 && *width != 8)) {
            error = "length prefix width must be 1, 2, 4 or 8";
            return std::nullopt;
        }
        if (parts[2] != "be" && parts[2] != "le") {
            error = "length prefix endianness must be be or le";
            return std::nullopt;
        }
        spec.kind = FramingKind::Length;
        spec.width = static_cast<std::uint8_t>(*width);
        spec.big_endian = parts[2] == "be";
        if (parts.size() == 4) {
            if (parts[3] == "incl" || parts[3] == "include" || parts[3] == "header") {
                spec.includes_header = true;
            } else if (parts[3] == "payload" || parts[3] == "body") {
                spec.includes_header = false;
            } else {
                error = "unknown length framing modifier: " + parts[3];
                return std::nullopt;
            }
        }
        return spec;
    }

    auto parse_typed_modifier = [&](const std::string& text, FramingSpec& target) -> bool {
        if (text == "incl" || text == "include" || text == "header") {
            target.includes_header = true;
            return true;
        }
        if (text == "payload" || text == "body") {
            target.includes_header = false;
            return true;
        }
        error = "unknown typed framing modifier: " + text;
        return false;
    };

    auto parse_typed_width = [](const std::string& text) -> std::optional<std::size_t> {
        const auto width = parse_size(text);
        if (!width || (*width != 1 && *width != 2 && *width != 4 && *width != 8)) {
            return std::nullopt;
        }
        return width;
    };

    if (!parts.empty() && parts[0] == "type") {
        // type:<type-width>:<be|le>+len:<length-width>:<be|le>[:incl|:payload]
        if (parts.size() < 5 || parts.size() > 6) {
            error = "typed framing requires "
                    "type:<1|2|4|8>:<be|le>+len:<1|2|4|8>:<be|le>[:incl|:payload]";
            return std::nullopt;
        }
        const auto type_width = parse_typed_width(parts[1]);
        if (!type_width) {
            error = "type prefix width must be 1, 2, 4 or 8";
            return std::nullopt;
        }
        if (parts[2] != "be+len" && parts[2] != "le+len") {
            error = "typed framing requires <be|le>+len between the two widths";
            return std::nullopt;
        }
        const auto length_width = parse_typed_width(parts[3]);
        if (!length_width) {
            error = "length prefix width must be 1, 2, 4 or 8";
            return std::nullopt;
        }
        if (parts[4] != "be" && parts[4] != "le") {
            error = "length prefix endianness must be be or le";
            return std::nullopt;
        }

        spec.kind = FramingKind::TypedLength;
        spec.type_width = static_cast<std::uint8_t>(*type_width);
        spec.type_big_endian = parts[2].starts_with("be");
        spec.width = static_cast<std::uint8_t>(*length_width);
        spec.big_endian = parts[4] == "be";
        if (parts.size() == 6 && !parse_typed_modifier(parts[5], spec)) {
            return std::nullopt;
        }
        return spec;
    }

    if (!parts.empty() && parts[0].starts_with("type") && parts[0].size() > 4) {
        // type<type-width>+len<length-width>[:<be|le>][:incl|:payload]
        // The type field defaults to big-endian; the length endianness is
        // mandatory in the shorthand form.
        if (parts.size() < 2 || parts.size() > 3) {
            error = "typed framing shorthand requires "
                    "type<n>+len<n>:<be|le>[:incl|:payload]";
            return std::nullopt;
        }
        const std::string& head = parts[0];
        const std::size_t plus = head.find("+len");
        if (plus == std::string::npos || plus <= 4) {
            error = "typed framing shorthand requires type<n>+len<n>:<be|le>";
            return std::nullopt;
        }
        const auto type_width = parse_typed_width(head.substr(4, plus - 4));
        if (!type_width) {
            error = "type prefix width must be 1, 2, 4 or 8";
            return std::nullopt;
        }
        const auto length_width = parse_typed_width(head.substr(plus + 4));
        if (!length_width) {
            error = "length prefix width must be 1, 2, 4 or 8";
            return std::nullopt;
        }
        if (parts[1] != "be" && parts[1] != "le") {
            error = "length prefix endianness must be be or le";
            return std::nullopt;
        }

        spec.kind = FramingKind::TypedLength;
        spec.type_width = static_cast<std::uint8_t>(*type_width);
        spec.type_big_endian = true;
        spec.width = static_cast<std::uint8_t>(*length_width);
        spec.big_endian = parts[1] == "be";
        if (parts.size() == 3 && !parse_typed_modifier(parts[2], spec)) {
            return std::nullopt;
        }
        return spec;
    }

    // Short aliases: u8, u16be, u32le, u64be, ...
    if (value.size() >= 2 && value[0] == 'u') {
        std::size_t pos = 1;
        std::size_t bits = 0;
        while (pos < value.size() && std::isdigit(static_cast<unsigned char>(value[pos]))) {
            bits = bits * 10 + static_cast<std::size_t>(value[pos] - '0');
            ++pos;
        }

        std::uint8_t width = 0;
        if (bits == 8) {
            width = 1;
        } else if (bits == 16) {
            width = 2;
        } else if (bits == 32) {
            width = 4;
        } else if (bits == 64) {
            width = 8;
        }

        if (width == 1 && pos == value.size()) {
            spec.kind = FramingKind::Length;
            spec.width = 1;
            spec.big_endian = true;
            return spec;
        }
        if (width > 1 && pos + 2 == value.size()) {
            const std::string_view endian = value.substr(pos);
            if (endian == "be" || endian == "le") {
                spec.kind = FramingKind::Length;
                spec.width = width;
                spec.big_endian = endian == "be";
                return spec;
            }
        }
    }

    error = "unknown framing: " + std::string(text);
    return std::nullopt;
}

} // namespace pr
