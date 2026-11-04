#pragma once

#include "wire/reader.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pr {

struct ParsedField {
    std::uint32_t number = 0;
    std::uint8_t wire_type = 0;
    std::uint64_t varint = 0;
    std::uint32_t fixed32 = 0;
    std::uint64_t fixed64 = 0;
    ByteSpan bytes;
    std::size_t encoded_size = 0;
};

struct ParsedMessage {
    std::vector<ParsedField> fields;
};

struct ParseLimits {
    std::size_t max_fields = 1'000'000;
    std::size_t max_depth = 64;
    std::size_t max_length = 16 * 1024 * 1024;
};

// Parses exactly one complete message. Trailing or truncated data is an error.
bool parse_message(ByteSpan data, ParsedMessage& out, const ParseLimits& limits,
                   int depth, std::string& error);

// Parses one top-level field. Returns false on a malformed field.
// complete=false means the field is not fully available yet; consumed is 0.
bool parse_one_field(ByteSpan data, ParsedField& out, std::size_t& consumed,
                     bool& complete, const ParseLimits& limits, std::string& error);

bool is_valid_utf8(ByteSpan data);
bool is_printable_utf8(ByteSpan data);

} // namespace pr
