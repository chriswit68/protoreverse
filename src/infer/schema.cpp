#include "infer/schema.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace pr {
namespace {

constexpr std::uint64_t kMaxNestedAttempts = 512;
constexpr std::uint64_t kMaxPackedAttempts = 512;
constexpr std::size_t kMaxProbeBytes = 64 * 1024;
constexpr std::size_t kMaxDistinctValues = 64;
constexpr std::size_t kMaxStringSamples = 8;
constexpr std::size_t kMaxStringSampleLength = 128;

bool parse_packed_varints(ByteSpan bytes, std::vector<std::uint64_t>& values) {
    values.clear();
    WireReader reader(bytes);
    while (reader.remaining() > 0) {
        std::uint64_t value = 0;
        if (reader.read_varint(value) != ReadResult::Ok) {
            return false;
        }
        values.push_back(value);
        if (values.size() > 1'000'000) {
            return false;
        }
    }
    return values.size() >= 2;
}

} // namespace

void NumericStats::add(std::uint64_t value) {
    ++count;
    min = std::min(min, value);
    max = std::max(max, value);
    if (value == 0) {
        ++zeros;
    }
    if (value == 1) {
        ++ones;
    }
    if ((value & (1ull << 63)) != 0) {
        ++high_bit;
    }

    if (!distinct_overflow) {
        if (distinct.size() < kMaxDistinctValues) {
            distinct.insert(value);
        } else if (distinct.find(value) == distinct.end()) {
            distinct_overflow = true;
        }
    }
}

bool plausible_float(std::uint32_t bits) {
    const float value = std::bit_cast<float>(bits);
    if (!std::isfinite(value)) {
        return false;
    }
    if (value == 0.0f) {
        return true;
    }
    const float magnitude = std::fabs(value);
    return magnitude >= 1e-30f && magnitude <= 1e12f;
}

bool plausible_double(std::uint64_t bits) {
    const double value = std::bit_cast<double>(bits);
    if (!std::isfinite(value)) {
        return false;
    }
    if (value == 0.0) {
        return true;
    }
    const double magnitude = std::fabs(value);
    return magnitude >= 1e-300 && magnitude <= 1e15;
}

SchemaStore::SchemaStore(bool raw_mode, bool keep_samples, ParseLimits limits)
    : raw_mode_(raw_mode), keep_samples_(keep_samples), limits_(limits) {}

void SchemaStore::add_message(const ParsedMessage& message) {
    merge_message_into(root_, message, 0, true);
}

void SchemaStore::add_raw_field(const ParsedField& field) {
    root_.message_count = 1;
    merge_field_into(root_, field, 0, false);
    root_.fields[field.number].messages_seen = 1;
}

void SchemaStore::merge_message_into(MessageSchema& schema, const ParsedMessage& message,
                                     int depth, bool increment_message_count) {
    if (increment_message_count) {
        ++schema.message_count;
    }

    std::set<std::uint32_t> seen_in_message;
    for (const ParsedField& field : message.fields) {
        const bool first = seen_in_message.insert(field.number).second;
        merge_field_into(schema, field, depth, first);
    }
}

void SchemaStore::merge_field_into(MessageSchema& schema, const ParsedField& field, int depth,
                                   bool first_in_message) {
    FieldStats& stats = schema.fields[field.number];
    stats.number = field.number;
    ++stats.occurrences;
    if (field.wire_type < stats.wire_counts.size()) {
        ++stats.wire_counts[field.wire_type];
    }
    if (first_in_message) {
        ++stats.messages_seen;
    }

    switch (field.wire_type) {
    case 0:
        stats.varint.add(field.varint);
        break;
    case 1:
        stats.fixed64.add(field.fixed64);
        if (plausible_double(field.fixed64)) {
            ++stats.double64_plausible;
        }
        break;
    case 2:
        observe_length(stats, field, depth);
        break;
    case 5:
        stats.fixed32.add(field.fixed32);
        if (plausible_float(field.fixed32)) {
            ++stats.float32_plausible;
        }
        break;
    default:
        break;
    }
}

void SchemaStore::observe_length(FieldStats& stats, const ParsedField& field, int depth) {
    const ByteSpan bytes = field.bytes;
    ++stats.len_total;
    stats.len_min = std::min(stats.len_min, static_cast<std::uint64_t>(bytes.size()));
    stats.len_max = std::max(stats.len_max, static_cast<std::uint64_t>(bytes.size()));

    if (is_valid_utf8(bytes)) {
        ++stats.utf8_ok;
        if (is_printable_utf8(bytes)) {
            ++stats.printable_ok;
            if (keep_samples_ && stats.string_samples.size() < kMaxStringSamples &&
                bytes.size() <= kMaxStringSampleLength) {
                stats.string_samples.emplace_back(
                    reinterpret_cast<const char*>(bytes.data()), bytes.size());
            }
        }
    }

    observe_nested(stats, bytes, depth);
    observe_packed(stats, bytes);
}

void SchemaStore::observe_nested(FieldStats& stats, ByteSpan bytes, int depth) {
    if (bytes.empty() || bytes.size() > kMaxProbeBytes ||
        stats.nested_attempts >= kMaxNestedAttempts) {
        return;
    }
    if (depth + 1 > static_cast<int>(limits_.max_depth)) {
        return;
    }

    ++stats.nested_attempts;

    ParsedMessage nested;
    std::string error;
    if (!parse_message(bytes, nested, limits_, depth + 1, error)) {
        return;
    }
    if (nested.fields.empty()) {
        return;
    }

    ++stats.nested_ok;
    if (!stats.nested) {
        stats.nested = std::make_unique<MessageSchema>();
    }
    merge_message_into(*stats.nested, nested, depth + 1, true);
}

void SchemaStore::observe_packed(FieldStats& stats, ByteSpan bytes) {
    if (bytes.empty() || bytes.size() > kMaxProbeBytes) {
        return;
    }

    if (bytes.size() >= 2 && stats.packed_varint_attempts < kMaxPackedAttempts) {
        ++stats.packed_varint_attempts;
        std::vector<std::uint64_t> values;
        if (parse_packed_varints(bytes, values)) {
            ++stats.packed_varint_ok;
            for (std::uint64_t value : values) {
                stats.packed_varints.add(value);
            }
        }
    }

    if (bytes.size() >= 4 && bytes.size() % 4 == 0 &&
        stats.packed_fixed32_attempts < kMaxPackedAttempts) {
        ++stats.packed_fixed32_attempts;
        ++stats.packed_fixed32_ok;
        for (std::size_t offset = 0; offset < bytes.size(); offset += 4) {
            const std::uint32_t value = decode_fixed32_le(bytes.subspan(offset, 4));
            stats.packed_fixed32.add(value);
            if (plausible_float(value)) {
                ++stats.packed_float32_plausible;
            }
        }
    }

    if (bytes.size() >= 8 && bytes.size() % 8 == 0 &&
        stats.packed_fixed64_attempts < kMaxPackedAttempts) {
        ++stats.packed_fixed64_attempts;
        ++stats.packed_fixed64_ok;
        for (std::size_t offset = 0; offset < bytes.size(); offset += 8) {
            const std::uint64_t value = decode_fixed64_le(bytes.subspan(offset, 8));
            stats.packed_fixed64.add(value);
            if (plausible_double(value)) {
                ++stats.packed_double64_plausible;
            }
        }
    }
}

} // namespace pr
