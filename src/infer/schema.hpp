#pragma once

#include "wire/validator.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace pr {

struct NumericStats {
    std::uint64_t count = 0;
    std::uint64_t min = UINT64_MAX;
    std::uint64_t max = 0;
    std::uint64_t zeros = 0;
    std::uint64_t ones = 0;
    std::uint64_t high_bit = 0;
    std::set<std::uint64_t> distinct;
    bool distinct_overflow = false;

    void add(std::uint64_t value);
};

struct MessageSchema;

struct FieldStats {
    std::uint32_t number = 0;
    std::array<std::uint64_t, 8> wire_counts{};
    std::uint64_t occurrences = 0;
    std::uint64_t messages_seen = 0;

    NumericStats varint;
    NumericStats packed_varints;

    NumericStats fixed32;
    NumericStats fixed64;
    NumericStats packed_fixed32;
    NumericStats packed_fixed64;

    std::uint64_t float32_plausible = 0;
    std::uint64_t double64_plausible = 0;
    std::uint64_t packed_float32_plausible = 0;
    std::uint64_t packed_double64_plausible = 0;

    std::uint64_t len_total = 0;
    std::uint64_t len_min = UINT64_MAX;
    std::uint64_t len_max = 0;
    std::uint64_t utf8_ok = 0;
    std::uint64_t printable_ok = 0;

    std::uint64_t nested_attempts = 0;
    std::uint64_t nested_ok = 0;
    std::uint64_t packed_varint_attempts = 0;
    std::uint64_t packed_varint_ok = 0;
    std::uint64_t packed_fixed32_attempts = 0;
    std::uint64_t packed_fixed32_ok = 0;
    std::uint64_t packed_fixed64_attempts = 0;
    std::uint64_t packed_fixed64_ok = 0;

    std::unique_ptr<MessageSchema> nested;
    std::vector<std::string> string_samples;
};

struct MessageSchema {
    std::uint64_t message_count = 0;
    std::map<std::uint32_t, FieldStats> fields;
};

class SchemaStore {
public:
    SchemaStore(bool raw_mode, bool keep_samples, ParseLimits limits);

    void add_message(const ParsedMessage& message);
    void add_raw_field(const ParsedField& field);

    MessageSchema& root() { return root_; }
    const MessageSchema& root() const { return root_; }
    bool raw_mode() const { return raw_mode_; }

private:
    void merge_message_into(MessageSchema& schema, const ParsedMessage& message, int depth,
                            bool increment_message_count);
    void merge_field_into(MessageSchema& schema, const ParsedField& field, int depth,
                          bool first_in_message);
    void observe_length(FieldStats& stats, const ParsedField& field, int depth);
    void observe_nested(FieldStats& stats, ByteSpan bytes, int depth);
    void observe_packed(FieldStats& stats, ByteSpan bytes);

    bool raw_mode_;
    bool keep_samples_;
    ParseLimits limits_;
    MessageSchema root_;
};

bool plausible_float(std::uint32_t bits);
bool plausible_double(std::uint64_t bits);

} // namespace pr
