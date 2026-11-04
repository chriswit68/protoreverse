#include "wire/reader.hpp"

#include <cstdio>

namespace pr {

ReadResult WireReader::read_varint(std::uint64_t& value) {
    std::uint64_t result = 0;
    int shift = 0;

    for (int i = 0; i < 10; ++i) {
        if (position_ >= data_.size()) {
            return ReadResult::NeedMore;
        }
        const std::uint8_t byte = data_[position_++];

        if (i == 9) {
            // The tenth byte may only contribute the 64th bit.
            if ((byte & 0x7e) != 0) {
                return ReadResult::Invalid;
            }
        }

        result |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            value = result;
            return ReadResult::Ok;
        }
        shift += 7;
    }

    return ReadResult::Invalid;
}

ReadResult WireReader::read_tag(std::uint32_t& field_number, std::uint8_t& wire_type,
                                std::string& error) {
    std::uint64_t key = 0;
    const ReadResult result = read_varint(key);
    if (result != ReadResult::Ok) {
        return result;
    }

    if (key == 0) {
        error = "invalid tag 0";
        return ReadResult::Invalid;
    }

    const std::uint32_t field = static_cast<std::uint32_t>(key >> 3);
    const std::uint8_t type = static_cast<std::uint8_t>(key & 0x07);

    if (field == 0) {
        error = "field number 0 is invalid";
        return ReadResult::Invalid;
    }
    if (field > 536870911u) {
        error = "field number exceeds protobuf maximum";
        return ReadResult::Invalid;
    }
    if (type == 6 || type == 7) {
        error = "invalid wire type " + std::to_string(type);
        return ReadResult::Invalid;
    }
    if (type == 3 || type == 4) {
        error = "deprecated group wire type is not supported";
        return ReadResult::Invalid;
    }

    field_number = field;
    wire_type = type;
    return ReadResult::Ok;
}

ReadResult WireReader::read_fixed32(std::uint32_t& value) {
    if (remaining() < 4) {
        return ReadResult::NeedMore;
    }
    value = decode_fixed32_le(data_.subspan(position_, 4));
    position_ += 4;
    return ReadResult::Ok;
}

ReadResult WireReader::read_fixed64(std::uint64_t& value) {
    if (remaining() < 8) {
        return ReadResult::NeedMore;
    }
    value = decode_fixed64_le(data_.subspan(position_, 8));
    position_ += 8;
    return ReadResult::Ok;
}

ReadResult WireReader::read_bytes(std::size_t length, ByteSpan& value) {
    if (remaining() < length) {
        return ReadResult::NeedMore;
    }
    value = data_.subspan(position_, length);
    position_ += length;
    return ReadResult::Ok;
}

std::uint32_t decode_fixed32_be(ByteSpan data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) |
           static_cast<std::uint32_t>(data[3]);
}

std::uint32_t decode_fixed32_le(ByteSpan data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint64_t decode_fixed64_be(ByteSpan data) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<std::uint64_t>(data[i]);
    }
    return value;
}

std::uint64_t decode_fixed64_le(ByteSpan data) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(data[i]) << (i * 8);
    }
    return value;
}

std::string varint_to_hex(std::uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%llx",
                  static_cast<unsigned long long>(value));
    return buffer;
}

} // namespace pr
