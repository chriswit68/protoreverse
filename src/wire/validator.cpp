#include "wire/validator.hpp"

#include <cstddef>

namespace pr {
namespace {

bool decode_utf8(ByteSpan data, std::size_t& pos, std::uint32_t& codepoint) {
    if (pos >= data.size()) {
        return false;
    }

    const std::uint8_t first = data[pos];
    if (first < 0x80) {
        codepoint = first;
        ++pos;
        return true;
    }

    int extra = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;

    if ((first & 0xe0) == 0xc0) {
        extra = 1;
        value = first & 0x1f;
        minimum = 0x80;
    } else if ((first & 0xf0) == 0xe0) {
        extra = 2;
        value = first & 0x0f;
        minimum = 0x800;
    } else if ((first & 0xf8) == 0xf0) {
        extra = 3;
        value = first & 0x07;
        minimum = 0x10000;
    } else {
        return false;
    }

    if (pos + static_cast<std::size_t>(extra) >= data.size()) {
        return false;
    }

    for (int i = 1; i <= extra; ++i) {
        const std::uint8_t byte = data[pos + static_cast<std::size_t>(i)];
        if ((byte & 0xc0) != 0x80) {
            return false;
        }
        value = (value << 6) | (byte & 0x3f);
    }

    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
        return false;
    }

    codepoint = value;
    pos += static_cast<std::size_t>(extra) + 1;
    return true;
}

bool is_printable_codepoint(std::uint32_t cp) {
    if (cp == '\t' || cp == '\n' || cp == '\r') {
        return true;
    }
    if (cp < 0x20 || cp == 0x7f) {
        return false;
    }
    if (cp >= 0x80 && cp <= 0x9f) {
        return false;
    }
    return true;
}

} // namespace

bool is_valid_utf8(ByteSpan data) {
    std::size_t pos = 0;
    while (pos < data.size()) {
        std::uint32_t cp = 0;
        if (!decode_utf8(data, pos, cp)) {
            return false;
        }
    }
    return true;
}

bool is_printable_utf8(ByteSpan data) {
    std::size_t pos = 0;
    while (pos < data.size()) {
        std::uint32_t cp = 0;
        if (!decode_utf8(data, pos, cp)) {
            return false;
        }
        if (!is_printable_codepoint(cp)) {
            return false;
        }
    }
    return true;
}

bool parse_one_field(ByteSpan data, ParsedField& out, std::size_t& consumed,
                     bool& complete, const ParseLimits& limits, std::string& error) {
    consumed = 0;
    complete = false;

    WireReader reader(data);
    ParsedField field;

    ReadResult result = reader.read_tag(field.number, field.wire_type, error);
    if (result == ReadResult::NeedMore) {
        return true;
    }
    if (result != ReadResult::Ok) {
        return false;
    }

    switch (field.wire_type) {
    case 0:
        result = reader.read_varint(field.varint);
        if (result == ReadResult::NeedMore) {
            return true;
        }
        if (result != ReadResult::Ok) {
            error = "invalid varint";
            return false;
        }
        break;
    case 1:
        result = reader.read_fixed64(field.fixed64);
        if (result == ReadResult::NeedMore) {
            return true;
        }
        if (result != ReadResult::Ok) {
            error = "truncated fixed64";
            return false;
        }
        break;
    case 2: {
        std::uint64_t length = 0;
        result = reader.read_varint(length);
        if (result == ReadResult::NeedMore) {
            return true;
        }
        if (result != ReadResult::Ok) {
            error = "invalid length varint";
            return false;
        }
        if (length > limits.max_length) {
            error = "length-delimited field exceeds maximum length";
            return false;
        }
        result = reader.read_bytes(static_cast<std::size_t>(length), field.bytes);
        if (result == ReadResult::NeedMore) {
            return true;
        }
        if (result != ReadResult::Ok) {
            error = "truncated length-delimited field";
            return false;
        }
        break;
    }
    case 5:
        result = reader.read_fixed32(field.fixed32);
        if (result == ReadResult::NeedMore) {
            return true;
        }
        if (result != ReadResult::Ok) {
            error = "truncated fixed32";
            return false;
        }
        break;
    default:
        error = "unsupported wire type";
        return false;
    }

    field.encoded_size = reader.position();
    consumed = reader.position();
    complete = true;
    out = field;
    return true;
}

bool parse_message(ByteSpan data, ParsedMessage& out, const ParseLimits& limits,
                   int depth, std::string& error) {
    if (depth > static_cast<int>(limits.max_depth)) {
        error = "maximum nesting depth exceeded";
        return false;
    }

    out.fields.clear();
    if (data.empty()) {
        return true;
    }

    std::size_t offset = 0;
    while (offset < data.size()) {
        ParsedField field;
        std::size_t consumed = 0;
        bool complete = false;
        if (!parse_one_field(data.subspan(offset), field, consumed, complete, limits, error)) {
            if (!error.empty()) {
                error = "at offset " + std::to_string(offset) + ": " + error;
            }
            return false;
        }
        if (!complete) {
            error = "truncated field at offset " + std::to_string(offset);
            return false;
        }
        if (out.fields.size() >= limits.max_fields) {
            error = "too many fields in message";
            return false;
        }
        out.fields.push_back(field);
        offset += consumed;
    }

    return true;
}

} // namespace pr
