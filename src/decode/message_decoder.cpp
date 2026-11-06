#include "decode/message_decoder.hpp"

#include "util/hexdump.hpp"
#include "wire/validator.hpp"

#include <bit>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <string_view>
#include <vector>

namespace pr {
namespace {

struct DecodeContext {
    const DecodeSchema& schema;
    const DecodeOptions& options;
    const ParseLimits& limits;
};

void append_json_string(std::string& out, std::string_view value) {
    out.push_back('"');
    for (char raw : value) {
        const unsigned char c = static_cast<unsigned char>(raw);
        switch (c) {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\b':
            out.append("\\b");
            break;
        case '\f':
            out.append("\\f");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            if (c < 0x20) {
                char buffer[7];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out.append(buffer);
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    out.push_back('"');
}

std::string format_float(float value) {
    if (std::isnan(value)) {
        return "\"NaN\"";
    }
    if (std::isinf(value)) {
        return value > 0 ? "\"Infinity\"" : "\"-Infinity\"";
    }
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
    return out.str();
}

std::string format_double(double value) {
    if (std::isnan(value)) {
        return "\"NaN\"";
    }
    if (std::isinf(value)) {
        return value > 0 ? "\"Infinity\"" : "\"-Infinity\"";
    }
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return out.str();
}

bool is_varint_type(std::string_view type) {
    return type == "bool" || type == "int32" || type == "int64" || type == "uint32" ||
           type == "uint64";
}

bool is_fixed32_type(std::string_view type) {
    return type == "fixed32" || type == "sfixed32" || type == "float";
}

bool is_fixed64_type(std::string_view type) {
    return type == "fixed64" || type == "sfixed64" || type == "double";
}

bool append_varint_value(std::string& out, std::string_view type, std::uint64_t value) {
    if (type == "bool") {
        out.append(value != 0 ? "true" : "false");
        return true;
    }
    if (type == "int32") {
        out.append(std::to_string(std::bit_cast<std::int32_t>(
            static_cast<std::uint32_t>(value))));
        return true;
    }
    if (type == "int64") {
        out.push_back('"');
        out.append(std::to_string(std::bit_cast<std::int64_t>(value)));
        out.push_back('"');
        return true;
    }
    if (type == "uint32") {
        out.append(std::to_string(static_cast<std::uint32_t>(value)));
        return true;
    }
    if (type == "uint64") {
        out.push_back('"');
        out.append(std::to_string(value));
        out.push_back('"');
        return true;
    }
    return false;
}

bool append_fixed32_value(std::string& out, std::string_view type, std::uint32_t value) {
    if (type == "fixed32") {
        out.append(std::to_string(value));
        return true;
    }
    if (type == "sfixed32") {
        out.append(std::to_string(std::bit_cast<std::int32_t>(value)));
        return true;
    }
    if (type == "float") {
        out.append(format_float(std::bit_cast<float>(value)));
        return true;
    }
    return false;
}

bool append_fixed64_value(std::string& out, std::string_view type, std::uint64_t value) {
    if (type == "fixed64") {
        out.push_back('"');
        out.append(std::to_string(value));
        out.push_back('"');
        return true;
    }
    if (type == "sfixed64") {
        out.push_back('"');
        out.append(std::to_string(std::bit_cast<std::int64_t>(value)));
        out.push_back('"');
        return true;
    }
    if (type == "double") {
        out.append(format_double(std::bit_cast<double>(value)));
        return true;
    }
    return false;
}

void append_raw_value(std::string& out, const ParsedField& field,
                      const DecodeOptions& options) {
    out.append("{\"wire_type\":");
    out.append(std::to_string(field.wire_type));

    switch (field.wire_type) {
    case 0:
        out.append(",\"value\":\"");
        out.append(std::to_string(field.varint));
        out.push_back('"');
        break;
    case 1:
        out.append(",\"value\":\"");
        out.append(std::to_string(field.fixed64));
        out.push_back('"');
        break;
    case 5:
        out.append(",\"value\":\"");
        out.append(std::to_string(field.fixed32));
        out.push_back('"');
        break;
    case 2:
        out.append(",\"raw\":");
        append_json_string(out, hexdump(field.bytes, options.hexdump_bytes));
        break;
    default:
        break;
    }
    out.push_back('}');
}

bool render_packed_values(const ParsedField& field, const ResolvedField& resolved,
                          const DecodeOptions&, std::string& rendered) {
    const ByteSpan data = field.bytes;
    if (is_varint_type(resolved.type)) {
        WireReader reader(data);
        bool first = true;
        while (reader.remaining() > 0) {
            std::uint64_t value = 0;
            if (reader.read_varint(value) != ReadResult::Ok) {
                return false;
            }
            if (!first) {
                rendered.push_back(',');
            }
            first = false;
            if (!append_varint_value(rendered, resolved.type, value)) {
                return false;
            }
        }
        return true;
    }

    if (is_fixed32_type(resolved.type)) {
        if (data.size() % 4 != 0) {
            return false;
        }
        for (std::size_t offset = 0; offset < data.size(); offset += 4) {
            if (offset > 0) {
                rendered.push_back(',');
            }
            const std::uint32_t value = decode_fixed32_le(data.subspan(offset, 4));
            if (!append_fixed32_value(rendered, resolved.type, value)) {
                return false;
            }
        }
        return true;
    }

    if (is_fixed64_type(resolved.type)) {
        if (data.size() % 8 != 0) {
            return false;
        }
        for (std::size_t offset = 0; offset < data.size(); offset += 8) {
            if (offset > 0) {
                rendered.push_back(',');
            }
            const std::uint64_t value = decode_fixed64_le(data.subspan(offset, 8));
            if (!append_fixed64_value(rendered, resolved.type, value)) {
                return false;
            }
        }
        return true;
    }

    return false;
}

bool append_message_fields(std::string& out, const ParsedMessage& message,
                           const ResolvedMessage& resolved,
                           const DecodeContext& context, int depth);

void append_nested_message(std::string& out, ByteSpan payload,
                           const ResolvedMessage& resolved,
                           const DecodeContext& context, int depth) {
    ParsedMessage nested;
    std::string error;
    if (!parse_message(payload, nested, context.limits, depth, error)) {
        append_json_string(out, hexdump(payload, context.options.hexdump_bytes));
        return;
    }

    out.push_back('{');
    if (!append_message_fields(out, nested, resolved, context, depth)) {
        out.push_back('}');
        return;
    }
    out.push_back('}');
}

void append_scalar_value(std::string& out, const ParsedField& field,
                         const ResolvedField& resolved, const DecodeContext& context,
                         int depth) {
    const auto message_it = context.schema.messages.find(resolved.type);
    if (message_it != context.schema.messages.end() && message_it->second != nullptr) {
        if (field.wire_type != 2) {
            append_raw_value(out, field, context.options);
            return;
        }
        std::string nested;
        append_nested_message(nested, field.bytes, *message_it->second, context, depth + 1);
        out.append(nested);
        return;
    }

    if (resolved.type == "string") {
        if (field.wire_type == 2 && is_valid_utf8(field.bytes)) {
            append_json_string(out, std::string_view(
                                        reinterpret_cast<const char*>(field.bytes.data()),
                                        field.bytes.size()));
        } else {
            append_raw_value(out, field, context.options);
        }
        return;
    }

    if (resolved.type == "bytes") {
        if (field.wire_type == 2) {
            append_json_string(out, hexdump(field.bytes, context.options.hexdump_bytes));
        } else {
            append_raw_value(out, field, context.options);
        }
        return;
    }

    if (is_varint_type(resolved.type)) {
        if (field.wire_type == 0 &&
            append_varint_value(out, resolved.type, field.varint)) {
            return;
        }
        append_raw_value(out, field, context.options);
        return;
    }

    if (is_fixed32_type(resolved.type)) {
        if (field.wire_type == 5 &&
            append_fixed32_value(out, resolved.type, field.fixed32)) {
            return;
        }
        append_raw_value(out, field, context.options);
        return;
    }

    if (is_fixed64_type(resolved.type)) {
        if (field.wire_type == 1 &&
            append_fixed64_value(out, resolved.type, field.fixed64)) {
            return;
        }
        append_raw_value(out, field, context.options);
        return;
    }

    append_raw_value(out, field, context.options);
}

void append_field_values(std::string& out, const std::vector<const ParsedField*>& fields,
                         const ResolvedField& resolved, const DecodeContext& context,
                         int depth) {
    const bool use_array = resolved.packed || resolved.repeated || fields.size() > 1;
    if (!use_array) {
        append_scalar_value(out, *fields.front(), resolved, context, depth);
        return;
    }

    out.push_back('[');
    bool first = true;
    for (const ParsedField* field : fields) {
        if (resolved.packed && field->wire_type == 2) {
            std::string packed;
            if (!render_packed_values(*field, resolved, context.options, packed)) {
                if (!first) {
                    out.push_back(',');
                }
                append_raw_value(out, *field, context.options);
                first = false;
                continue;
            }
            if (!packed.empty()) {
                if (!first) {
                    out.push_back(',');
                }
                out.append(packed);
                first = false;
            }
            continue;
        }

        if (!first) {
            out.push_back(',');
        }
        append_scalar_value(out, *field, resolved, context, depth);
        first = false;
    }
    out.push_back(']');
}

void append_raw_field(std::string& out, const std::vector<const ParsedField*>& fields,
                      const DecodeOptions& options) {
    if (fields.size() == 1) {
        append_raw_value(out, *fields.front(), options);
        return;
    }

    out.push_back('[');
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        append_raw_value(out, *fields[i], options);
    }
    out.push_back(']');
}

bool append_message_fields(std::string& out, const ParsedMessage& message,
                           const ResolvedMessage& resolved,
                           const DecodeContext& context, int depth) {
    if (depth > static_cast<int>(context.limits.max_depth)) {
        return false;
    }

    std::map<std::uint32_t, const ResolvedField*> resolved_fields;
    for (const ResolvedField& field : resolved.fields) {
        resolved_fields[field.number] = &field;
    }

    std::map<std::uint32_t, std::vector<const ParsedField*>> observed;
    for (const ParsedField& field : message.fields) {
        observed[field.number].push_back(&field);
    }

    bool first_field = true;
    for (const auto& [number, fields] : observed) {
        if (!first_field) {
            out.push_back(',');
        }
        first_field = false;

        out.push_back('"');
        out.append("field_");
        out.append(std::to_string(number));
        out.append("\":");

        const auto it = resolved_fields.find(number);
        if (it == resolved_fields.end() || it->second == nullptr || !it->second->valid) {
            append_raw_field(out, fields, context.options);
        } else {
            append_field_values(out, fields, *it->second, context, depth);
        }
    }

    return true;
}

} // namespace

DecodeSchema make_decode_schema(const ResolvedModel& model) {
    DecodeSchema schema;
    for (const ResolvedMessage& message : model.messages) {
        schema.messages[message.name] = &message;
    }

    const auto it = schema.messages.find(model.root_name);
    if (it != schema.messages.end()) {
        schema.root = it->second;
    }
    return schema;
}

void render_message_json(ByteSpan payload, const DecodeSchema& schema,
                         const MessageMetadata& metadata, const DecodeOptions& options,
                         const ParseLimits& limits, std::string& out) {
    out.clear();
    out.append("{\"session\":");
    out.append(std::to_string(metadata.session));
    out.append(",\"index\":");
    out.append(std::to_string(metadata.index));
    if (metadata.frame_type.has_value()) {
        out.append(",\"type\":");
        out.append(std::to_string(*metadata.frame_type));
    }

    if (schema.root == nullptr) {
        out.append(",\"error\":\"no inferred schema available\",\"raw\":");
        append_json_string(out, hexdump(payload, options.hexdump_bytes));
        out.push_back('}');
        return;
    }

    ParsedMessage message;
    std::string error;
    if (!parse_message(payload, message, limits, 0, error)) {
        out.append(",\"error\":");
        append_json_string(out, error);
        out.append(",\"raw\":");
        append_json_string(out, hexdump(payload, options.hexdump_bytes));
        out.push_back('}');
        return;
    }

    out.append(",\"fields\":{");
    const DecodeContext context{schema, options, limits};
    if (!append_message_fields(out, message, *schema.root, context, 0)) {
        out.resize(out.size() - 1);
        out.append("{\"_error\":\"maximum nesting depth exceeded\"}");
    }
    out.append("}}");
}

} // namespace pr
