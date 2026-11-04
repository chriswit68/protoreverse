#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace pr {

using ByteSpan = std::span<const std::uint8_t>;

enum class ReadResult {
    Ok,
    NeedMore,
    Invalid,
};

class WireReader {
public:
    explicit WireReader(ByteSpan data) : data_(data) {}

    ReadResult read_varint(std::uint64_t& value);
    ReadResult read_tag(std::uint32_t& field_number, std::uint8_t& wire_type,
                        std::string& error);
    ReadResult read_fixed32(std::uint32_t& value);
    ReadResult read_fixed64(std::uint64_t& value);
    ReadResult read_bytes(std::size_t length, ByteSpan& value);

    std::size_t position() const { return position_; }
    std::size_t remaining() const { return data_.size() - position_; }
    ByteSpan remaining_bytes() const { return data_.subspan(position_); }
    ByteSpan data() const { return data_; }

private:
    ByteSpan data_;
    std::size_t position_ = 0;
};

std::uint32_t decode_fixed32_be(ByteSpan data);
std::uint32_t decode_fixed32_le(ByteSpan data);
std::uint64_t decode_fixed64_be(ByteSpan data);
std::uint64_t decode_fixed64_le(ByteSpan data);

std::string varint_to_hex(std::uint64_t value);

} // namespace pr
