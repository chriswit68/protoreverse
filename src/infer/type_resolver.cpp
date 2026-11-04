#include "infer/type_resolver.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace pr {
namespace {

struct TypeChoice {
    std::string type;
    double confidence = 0.5;
    std::string note;
};

std::string format_confidence(double value) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    out << value;
    return out.str();
}

std::string wire_type_names(const FieldStats& stats) {
    std::string out;
    auto append = [&](std::size_t index, const char* name) {
        if (stats.wire_counts[index] == 0) {
            return;
        }
        if (!out.empty()) {
            out += ", ";
        }
        out += name;
        out += "=";
        out += std::to_string(stats.wire_counts[index]);
    };
    append(0, "varint");
    append(1, "fixed64");
    append(2, "length");
    append(5, "fixed32");
    return out;
}

NumericStats combine(const NumericStats& a, const NumericStats& b) {
    if (b.count == 0) {
        return a;
    }
    if (a.count == 0) {
        return b;
    }

    NumericStats out = a;
    out.count += b.count;
    out.min = std::min(out.min, b.min);
    out.max = std::max(out.max, b.max);
    out.zeros += b.zeros;
    out.ones += b.ones;
    out.high_bit += b.high_bit;

    if (!out.distinct_overflow) {
        for (std::uint64_t value : b.distinct) {
            if (out.distinct.size() < 64) {
                out.distinct.insert(value);
            } else if (out.distinct.find(value) == out.distinct.end()) {
                out.distinct_overflow = true;
                break;
            }
        }
        if (b.distinct_overflow) {
            out.distinct_overflow = true;
        }
    }
    return out;
}

TypeChoice choose_varint_type(const NumericStats& stats) {
    TypeChoice choice;
    choice.type = "int32";

    if (stats.count == 0) {
        choice.confidence = 0.3;
        choice.note = "no numeric samples";
        return choice;
    }

    if (stats.count >= 3 && stats.zeros + stats.ones == stats.count) {
        choice.type = "bool";
        choice.confidence = 0.8;
        choice.note = "values are only 0/1; int32 is also possible";
        if (!stats.distinct_overflow && stats.distinct.size() <= 1) {
            choice.confidence = 0.7;
            choice.note += "; only one distinct value observed";
        }
        return choice;
    }

    if (stats.high_bit > 0) {
        choice.type = "int64";
        choice.confidence = 0.8;
        choice.note = "negative varints present; int32 possible if values fit; uint64 also possible";
        return choice;
    }

    if (stats.max <= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
        choice.type = "int32";
        choice.confidence = 0.85;
        if (!stats.distinct_overflow && stats.distinct.size() <= 16 && stats.distinct.size() >= 2) {
            choice.note = "enum candidate with " + std::to_string(stats.distinct.size()) +
                          " distinct values; sint32/sint64 also wire-compatible";
        } else {
            choice.note = "sint32/sint64 also wire-compatible";
        }
        return choice;
    }

    if (stats.max <= std::numeric_limits<std::uint32_t>::max()) {
        choice.type = "uint32";
        choice.confidence = 0.7;
        choice.note = "values exceed int32; int64/uint64 also possible";
        return choice;
    }

    choice.type = "int64";
    choice.confidence = 0.7;
    choice.note = "uint64 also possible";
    return choice;
}

TypeChoice choose_fixed32_type(const NumericStats& stats, std::uint64_t plausible) {
    TypeChoice choice;
    choice.type = "fixed32";
    choice.confidence = 0.8;

    if (stats.count >= 3 && plausible > 0 && plausible * 5 >= stats.count * 4 &&
        stats.max > 1'000'000) {
        choice.type = "float";
        choice.confidence = 0.75;
        choice.note = "fixed32 also possible";
        return choice;
    }

    if (plausible > 0) {
        choice.note = "float possible for some values";
    }
    if (stats.high_bit > 0) {
        if (!choice.note.empty()) {
            choice.note += "; ";
        }
        choice.note += "sfixed32 possible";
    }
    return choice;
}

TypeChoice choose_fixed64_type(const NumericStats& stats, std::uint64_t plausible) {
    TypeChoice choice;
    choice.type = "fixed64";
    choice.confidence = 0.8;

    if (stats.count >= 3 && plausible > 0 && plausible * 5 >= stats.count * 4 &&
        stats.max > 1'000'000'000) {
        choice.type = "double";
        choice.confidence = 0.75;
        choice.note = "fixed64 also possible";
        return choice;
    }

    if (plausible > 0) {
        choice.note = "double possible for some values";
    }
    if (stats.high_bit > 0) {
        if (!choice.note.empty()) {
            choice.note += "; ";
        }
        choice.note += "sfixed64 possible";
    }
    return choice;
}

double sample_factor(std::uint64_t count) {
    if (count == 0) {
        return 0.35;
    }
    const double value = std::log10(1.0 + static_cast<double>(count));
    return std::clamp(0.35 + 0.65 * (value / 2.0), 0.35, 1.0);
}

bool reserved_field_number(std::uint32_t number) {
    return number >= 19000 && number <= 19999;
}

bool message_like(const FieldStats& stats) {
    if (!stats.nested || stats.nested->fields.empty() || stats.nested_attempts == 0) {
        return false;
    }
    const double ratio =
        static_cast<double>(stats.nested_ok) / static_cast<double>(stats.nested_attempts);
    if (stats.nested_ok >= 2 && ratio >= 0.75) {
        return true;
    }
    if (stats.nested_ok == 1 && stats.nested->fields.size() >= 2 && stats.utf8_ok == 0) {
        return true;
    }
    if (stats.nested_ok >= 1 && ratio >= 0.9 && stats.printable_ok == 0) {
        return true;
    }
    return false;
}

bool string_like(const FieldStats& stats) {
    if (stats.len_total == 0) {
        return false;
    }
    return stats.utf8_ok == stats.len_total &&
           stats.printable_ok * 5 >= stats.len_total * 4;
}

// A packed repeated field may be the only encoding observed for a field, so
// "occurrences > messages_seen" is not a valid signal here: packed fields
// appear at most once per message. Instead require repeated successful packed
// decodes and enough decoded values to make the guess worthwhile.
bool packed_varint_like(const FieldStats& stats) {
    if (stats.packed_varint_attempts == 0 || stats.packed_varint_ok < 2 ||
        stats.len_total < 2 || stats.packed_varints.count < 3) {
        return false;
    }
    const double ratio = static_cast<double>(stats.packed_varint_ok) /
                         static_cast<double>(stats.packed_varint_attempts);
    return ratio >= 0.8;
}

bool packed_fixed32_like(const FieldStats& stats) {
    if (stats.packed_fixed32_attempts == 0 || stats.packed_fixed32_ok < 2 ||
        stats.len_total < 2 || stats.packed_fixed32.count < 3) {
        return false;
    }
    const double ratio = static_cast<double>(stats.packed_fixed32_ok) /
                         static_cast<double>(stats.packed_fixed32_attempts);
    return ratio >= 0.8;
}

bool packed_fixed64_like(const FieldStats& stats) {
    if (stats.packed_fixed64_attempts == 0 || stats.packed_fixed64_ok < 2 ||
        stats.len_total < 2 || stats.packed_fixed64.count < 3) {
        return false;
    }
    const double ratio = static_cast<double>(stats.packed_fixed64_ok) /
                         static_cast<double>(stats.packed_fixed64_attempts);
    return ratio >= 0.8;
}

class Resolver {
public:
    Resolver(const ResolveOptions& options, ResolvedModel& model)
        : options_(options), model_(model) {}

    std::string resolve_message(const MessageSchema& schema, const std::string& name,
                                int depth, const std::string& comment) {
        if (depth > 64) {
            return name;
        }

        ResolvedMessage message;
        message.name = name;
        message.comment = comment;

        for (const auto& [number, stats] : schema.fields) {
            if (stats.occurrences == 0) {
                continue;
            }

            ResolvedField field;
            field.number = number;
            field.name = "field_" + std::to_string(number);
            field.valid = !reserved_field_number(number);

            const bool has_varint = stats.wire_counts[0] > 0;
            const bool has_fixed64 = stats.wire_counts[1] > 0;
            const bool has_len = stats.wire_counts[2] > 0;
            const bool has_fixed32 = stats.wire_counts[5] > 0;

            const std::size_t active_wire_types = static_cast<std::size_t>(has_varint) +
                                                  static_cast<std::size_t>(has_fixed64) +
                                                  static_cast<std::size_t>(has_len) +
                                                  static_cast<std::size_t>(has_fixed32);

            TypeChoice choice;
            bool is_message = false;

            if (!field.valid) {
                choice.type = "bytes";
                choice.confidence = 0.0;
                choice.note = "field number is reserved by protobuf";
            } else if (has_varint && has_len && stats.packed_varint_ok > 0 &&
                       stats.packed_varint_ok * 5 >= stats.packed_varint_attempts * 4) {
                const NumericStats combined = combine(stats.varint, stats.packed_varints);
                choice = choose_varint_type(combined);
                field.repeated = true;
                field.packed = true;
                choice.note += "; packed repeated field";
            } else if (has_fixed32 && has_len && stats.packed_fixed32_ok > 0 &&
                       stats.packed_fixed32_ok * 5 >= stats.packed_fixed32_attempts * 4) {
                const NumericStats combined = combine(stats.fixed32, stats.packed_fixed32);
                choice = choose_fixed32_type(combined, stats.float32_plausible +
                                                          stats.packed_float32_plausible);
                field.repeated = true;
                field.packed = true;
                choice.note += "; packed repeated field";
            } else if (has_fixed64 && has_len && stats.packed_fixed64_ok > 0 &&
                       stats.packed_fixed64_ok * 5 >= stats.packed_fixed64_attempts * 4) {
                const NumericStats combined = combine(stats.fixed64, stats.packed_fixed64);
                choice = choose_fixed64_type(combined, stats.double64_plausible +
                                                          stats.packed_double64_plausible);
                field.repeated = true;
                field.packed = true;
                choice.note += "; packed repeated field";
            } else if (has_len && message_like(stats)) {
                const std::string child_name =
                    name + "_Field" + std::to_string(number);
                const std::string child_comment =
                    "nested message inferred from field " + std::to_string(number);
                field.type = resolve_message(*stats.nested, child_name, depth + 1, child_comment);
                field.repeated = stats.occurrences > stats.messages_seen;
                is_message = true;
                choice.confidence = 0.9;
                choice.note = "nested message";
            } else if (has_len && string_like(stats)) {
                choice.type = "string";
                choice.confidence = 0.55 + 0.4 * (static_cast<double>(stats.printable_ok) /
                                                  static_cast<double>(stats.len_total));
                choice.note = "UTF-8/printable";
                if (stats.len_max == 0) {
                    choice.note += "; all values empty, bytes/message possible";
                }
            } else if (has_len && packed_varint_like(stats)) {
                choice = choose_varint_type(stats.packed_varints);
                field.repeated = true;
                field.packed = true;
                choice.confidence *= 0.85;
                choice.note += "; packed repeated field; packed-only inference is weaker";
                if (packed_fixed32_like(stats) || packed_fixed64_like(stats)) {
                    choice.note += "; packed fixed-width numbers also possible";
                }
            } else if (has_len && packed_fixed32_like(stats)) {
                choice = choose_fixed32_type(stats.packed_fixed32,
                                             stats.packed_float32_plausible);
                field.repeated = true;
                field.packed = true;
                choice.confidence *= 0.85;
                choice.note += "; packed repeated field; packed-only inference is weaker";
                if (packed_fixed64_like(stats)) {
                    choice.note += "; packed fixed64/double also possible";
                }
            } else if (has_len && packed_fixed64_like(stats)) {
                choice = choose_fixed64_type(stats.packed_fixed64,
                                             stats.packed_double64_plausible);
                field.repeated = true;
                field.packed = true;
                choice.confidence *= 0.85;
                choice.note += "; packed repeated field; packed-only inference is weaker";
            } else if (has_len) {
                choice.type = "bytes";
                choice.confidence = 0.5;
                choice.note = "not UTF-8 and no stable nested message structure";
                if (stats.nested_ok > 0) {
                    choice.note += "; nested message possible";
                }
            } else if (has_varint) {
                const NumericStats combined =
                    combine(stats.varint, stats.packed_varints);
                choice = choose_varint_type(combined);
            } else if (has_fixed32) {
                const NumericStats combined =
                    combine(stats.fixed32, stats.packed_fixed32);
                choice = choose_fixed32_type(combined, stats.float32_plausible +
                                                          stats.packed_float32_plausible);
            } else if (has_fixed64) {
                const NumericStats combined =
                    combine(stats.fixed64, stats.packed_fixed64);
                choice = choose_fixed64_type(combined, stats.double64_plausible +
                                                          stats.packed_double64_plausible);
            } else {
                choice.type = "bytes";
                choice.confidence = 0.3;
                choice.note = "no usable wire type information";
            }

            if (!is_message) {
                field.type = choice.type;
            }
            if (!field.packed) {
                field.repeated = stats.occurrences > stats.messages_seen;
            }

            if (active_wire_types > 1 && !(field.packed && (has_varint || has_fixed32 || has_fixed64))) {
                if (!choice.note.empty()) {
                    choice.note += "; ";
                }
                choice.note += "conflicting wire types: " + wire_type_names(stats);
            }

            if (options_.raw_mode) {
                if (!choice.note.empty()) {
                    choice.note += "; ";
                }
                if (field.repeated) {
                    choice.note +=
                        "RAW mode: repeated occurrences may belong to different logical "
                        "messages; cardinality is unreliable";
                } else {
                    choice.note += "RAW mode: cardinality is unreliable";
                }
            }

            const std::uint64_t sample_count =
                std::max<std::uint64_t>(stats.messages_seen, stats.occurrences);
            field.confidence =
                std::clamp(choice.confidence * sample_factor(sample_count), 0.0, 0.99);

            std::ostringstream comment;
            comment << stats.occurrences << " occurrence" << (stats.occurrences == 1 ? "" : "s")
                    << " in " << stats.messages_seen << " message"
                    << (stats.messages_seen == 1 ? "" : "s")
                    << "; confidence " << format_confidence(field.confidence);
            if (!choice.note.empty()) {
                comment << "; " << choice.note;
            }
            field.comment = comment.str();

            ++model_.field_count;
            message.fields.push_back(std::move(field));
        }

        model_.messages.push_back(std::move(message));
        return name;
    }

private:
    const ResolveOptions& options_;
    ResolvedModel& model_;
};

} // namespace

ResolvedModel resolve_schema(const MessageSchema& root, const ResolveOptions& options) {
    ResolvedModel model;
    model.root_name = options.root_name;
    model.raw_mode = options.raw_mode;
    model.message_count = root.message_count;

    Resolver resolver(options, model);
    std::string root_comment;
    if (options.raw_mode) {
        root_comment =
            "RAW mode: the continuous stream was analyzed as one merged logical message; "
            "repeated/cardinality inference is unreliable";
    }
    resolver.resolve_message(root, options.root_name, 0, root_comment);
    return model;
}

} // namespace pr
