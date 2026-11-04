#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace pr {

void append_varint(std::vector<std::uint8_t>& out, std::uint64_t value);
void append_tag(std::vector<std::uint8_t>& out, std::uint32_t field_number,
                std::uint8_t wire_type);
void append_fixed32(std::vector<std::uint8_t>& out, std::uint32_t value);
void append_fixed64(std::vector<std::uint8_t>& out, std::uint64_t value);
void append_varint_field(std::vector<std::uint8_t>& out, std::uint32_t field_number,
                         std::uint64_t value);
void append_fixed32_field(std::vector<std::uint8_t>& out, std::uint32_t field_number,
                          std::uint32_t value);
void append_fixed64_field(std::vector<std::uint8_t>& out, std::uint32_t field_number,
                          std::uint64_t value);
void append_bytes_field(std::vector<std::uint8_t>& out, std::uint32_t field_number,
                        std::span<const std::uint8_t> value);
void append_string_field(std::vector<std::uint8_t>& out, std::uint32_t field_number,
                         std::string_view value);

} // namespace pr
