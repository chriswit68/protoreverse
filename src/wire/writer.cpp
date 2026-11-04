#include "wire/writer.hpp"

namespace pr {

void append_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<std::uint8_t>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

void append_tag(std::vector<std::uint8_t>& out, std::uint32_t field_number,
                std::uint8_t wire_type) {
    append_varint(out, (static_cast<std::uint64_t>(field_number) << 3) | wire_type);
}

void append_fixed32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xff));
    }
}

void append_fixed64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xff));
    }
}

void append_varint_field(std::vector<std::uint8_t>& out, std::uint32_t field_number,
                         std::uint64_t value) {
    append_tag(out, field_number, 0);
    append_varint(out, value);
}

void append_fixed32_field(std::vector<std::uint8_t>& out, std::uint32_t field_number,
                          std::uint32_t value) {
    append_tag(out, field_number, 5);
    append_fixed32(out, value);
}

void append_fixed64_field(std::vector<std::uint8_t>& out, std::uint32_t field_number,
                          std::uint64_t value) {
    append_tag(out, field_number, 1);
    append_fixed64(out, value);
}

void append_bytes_field(std::vector<std::uint8_t>& out, std::uint32_t field_number,
                        std::span<const std::uint8_t> value) {
    append_tag(out, field_number, 2);
    append_varint(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

void append_string_field(std::vector<std::uint8_t>& out, std::uint32_t field_number,
                         std::string_view value) {
    append_bytes_field(out, field_number,
                       std::span<const std::uint8_t>(
                           reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
}

} // namespace pr
