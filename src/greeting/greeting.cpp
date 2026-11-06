#include "greeting/greeting.hpp"

#include "framing/encoder.hpp"
#include "wire/reader.hpp"
#include "wire/writer.hpp"

#include <bit>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <system_error>

namespace pr {
namespace {

std::string lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string_view trim_view(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool supported_type(std::string_view type) {
    return type == "int32" || type == "int64" || type == "uint32" || type == "uint64" ||
           type == "sint32" || type == "sint64" || type == "bool" || type == "string" ||
           type == "bytes" || type == "fixed32" || type == "fixed64" || type == "float" ||
           type == "double";
}

bool parse_i64(std::string_view text, std::int64_t& value, std::string& error) {
    if (text.empty()) {
        error = "empty integer value";
        return false;
    }
    std::size_t start = 0;
    if (text[0] == '+') {
        start = 1;
    }
    if (start == text.size()) {
        error = "invalid integer value: " + std::string(text);
        return false;
    }
    const auto [ptr, ec] =
        std::from_chars(text.data() + start, text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        error = "invalid integer value: " + std::string(text);
        return false;
    }
    return true;
}

bool parse_u64(std::string_view text, std::uint64_t& value, std::string& error) {
    if (text.empty()) {
        error = "empty unsigned integer value";
        return false;
    }
    std::size_t start = 0;
    if (text[0] == '+') {
        start = 1;
    }
    if (start == text.size() || text[start] == '-') {
        error = "invalid unsigned integer value: " + std::string(text);
        return false;
    }
    const auto [ptr, ec] =
        std::from_chars(text.data() + start, text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        error = "invalid unsigned integer value: " + std::string(text);
        return false;
    }
    return true;
}

bool parse_float(std::string_view text, float& value, std::string& error) {
    if (text.empty()) {
        error = "empty float value";
        return false;
    }
    const auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        error = "invalid float value: " + std::string(text);
        return false;
    }
    return true;
}

bool parse_double(std::string_view text, double& value, std::string& error) {
    if (text.empty()) {
        error = "empty double value";
        return false;
    }
    const auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        error = "invalid double value: " + std::string(text);
        return false;
    }
    return true;
}

bool parse_bool(std::string_view text, bool& value, std::string& error) {
    const std::string cleaned = lower(trim_view(text));
    if (cleaned == "1" || cleaned == "true" || cleaned == "yes") {
        value = true;
        return true;
    }
    if (cleaned == "0" || cleaned == "false" || cleaned == "no") {
        value = false;
        return true;
    }
    error = "invalid bool value: " + std::string(text);
    return false;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool starts_with_ci(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

bool parse_hex_bytes(std::string_view text, std::vector<std::uint8_t>& bytes,
                     std::string& error) {
    bytes.clear();
    std::size_t pos = 0;
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
        ++pos;
    }
    if (pos + 1 < text.size() && text[pos] == '0' &&
        (text[pos + 1] == 'x' || text[pos + 1] == 'X')) {
        pos += 2;
    }

    int high = -1;
    for (; pos < text.size(); ++pos) {
        const char c = text[pos];
        if (std::isspace(static_cast<unsigned char>(c)) || c == ':' || c == '-' ||
            c == ',' || c == '_') {
            continue;
        }
        const int digit = hex_digit(c);
        if (digit < 0) {
            error = "invalid hex character: " + std::string(1, c);
            return false;
        }
        if (high < 0) {
            high = digit;
        } else {
            bytes.push_back(static_cast<std::uint8_t>((high << 4) | digit));
            high = -1;
        }
    }

    if (high >= 0) {
        error = "hex payload contains an odd number of digits";
        return false;
    }
    if (bytes.empty()) {
        error = "hex payload is empty";
        return false;
    }
    return true;
}

bool parse_bytes(std::string_view text, std::vector<std::uint8_t>& bytes,
                 std::string& error) {
    if (starts_with_ci(text, "hex:")) {
        return parse_hex_bytes(text.substr(4), bytes, error);
    }
    if (starts_with_ci(text, "0x")) {
        return parse_hex_bytes(text, bytes, error);
    }
    bytes.assign(text.begin(), text.end());
    return true;
}

bool read_file(const std::string& path, std::size_t max_payload,
               std::vector<std::uint8_t>& payload, std::string& error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = "cannot open greeting file: " + path;
        return false;
    }

    const std::streamoff size = file.tellg();
    if (size < 0) {
        error = "cannot determine size of greeting file: " + path;
        return false;
    }
    if (static_cast<std::uint64_t>(size) > max_payload) {
        error = "greeting file exceeds maximum payload size";
        return false;
    }

    payload.resize(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!payload.empty()) {
        file.read(reinterpret_cast<char*>(payload.data()),
                  static_cast<std::streamsize>(payload.size()));
        if (!file) {
            error = "cannot read greeting file: " + path;
            return false;
        }
    }
    return true;
}

bool encode_field(const GreetingField& field, std::vector<std::uint8_t>& out,
                  std::string& error) {
    const std::string& type = field.type;
    const std::string& value = field.value;

    if (type == "int32") {
        std::int64_t parsed = 0;
        if (!parse_i64(value, parsed, error)) {
            return false;
        }
        if (parsed < std::numeric_limits<std::int32_t>::min() ||
            parsed > std::numeric_limits<std::int32_t>::max()) {
            error = "int32 value out of range: " + value;
            return false;
        }
        const std::int64_t widened = static_cast<std::int64_t>(static_cast<std::int32_t>(parsed));
        append_varint_field(out, field.number, static_cast<std::uint64_t>(widened));
        return true;
    }

    if (type == "int64") {
        std::int64_t parsed = 0;
        if (!parse_i64(value, parsed, error)) {
            return false;
        }
        append_varint_field(out, field.number, static_cast<std::uint64_t>(parsed));
        return true;
    }

    if (type == "uint32") {
        std::uint64_t parsed = 0;
        if (!parse_u64(value, parsed, error)) {
            return false;
        }
        if (parsed > std::numeric_limits<std::uint32_t>::max()) {
            error = "uint32 value out of range: " + value;
            return false;
        }
        append_varint_field(out, field.number, parsed);
        return true;
    }

    if (type == "uint64") {
        std::uint64_t parsed = 0;
        if (!parse_u64(value, parsed, error)) {
            return false;
        }
        append_varint_field(out, field.number, parsed);
        return true;
    }

    if (type == "sint32") {
        std::int64_t parsed = 0;
        if (!parse_i64(value, parsed, error)) {
            return false;
        }
        if (parsed < std::numeric_limits<std::int32_t>::min() ||
            parsed > std::numeric_limits<std::int32_t>::max()) {
            error = "sint32 value out of range: " + value;
            return false;
        }
        const std::uint32_t raw = static_cast<std::uint32_t>(static_cast<std::int32_t>(parsed));
        const std::uint32_t zigzag = (raw << 1) ^ (0u - (raw >> 31));
        append_varint_field(out, field.number, zigzag);
        return true;
    }

    if (type == "sint64") {
        std::int64_t parsed = 0;
        if (!parse_i64(value, parsed, error)) {
            return false;
        }
        const std::uint64_t raw = static_cast<std::uint64_t>(parsed);
        const std::uint64_t zigzag = (raw << 1) ^ (std::uint64_t{0} - (raw >> 63));
        append_varint_field(out, field.number, zigzag);
        return true;
    }

    if (type == "bool") {
        bool parsed = false;
        if (!parse_bool(value, parsed, error)) {
            return false;
        }
        append_varint_field(out, field.number, parsed ? 1 : 0);
        return true;
    }

    if (type == "fixed32") {
        std::uint64_t parsed = 0;
        if (!parse_u64(value, parsed, error)) {
            return false;
        }
        if (parsed > std::numeric_limits<std::uint32_t>::max()) {
            error = "fixed32 value out of range: " + value;
            return false;
        }
        append_fixed32_field(out, field.number, static_cast<std::uint32_t>(parsed));
        return true;
    }

    if (type == "fixed64") {
        std::uint64_t parsed = 0;
        if (!parse_u64(value, parsed, error)) {
            return false;
        }
        append_fixed64_field(out, field.number, parsed);
        return true;
    }

    if (type == "float") {
        float parsed = 0.0f;
        if (!parse_float(value, parsed, error)) {
            return false;
        }
        append_fixed32_field(out, field.number, std::bit_cast<std::uint32_t>(parsed));
        return true;
    }

    if (type == "double") {
        double parsed = 0.0;
        if (!parse_double(value, parsed, error)) {
            return false;
        }
        append_fixed64_field(out, field.number, std::bit_cast<std::uint64_t>(parsed));
        return true;
    }

    if (type == "string") {
        append_string_field(out, field.number, value);
        return true;
    }

    if (type == "bytes") {
        std::vector<std::uint8_t> parsed;
        if (!parse_bytes(value, parsed, error)) {
            return false;
        }
        append_bytes_field(out, field.number, parsed);
        return true;
    }

    error = "unsupported greeting field type: " + type;
    return false;
}

} // namespace

bool parse_greeting_field(std::string_view text, GreetingField& field, std::string& error) {
    error.clear();
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        error = "expected <field-number>:<type>=<value>";
        return false;
    }
    const std::size_t equals = text.find('=', colon + 1);
    if (equals == std::string_view::npos) {
        error = "expected <field-number>:<type>=<value>";
        return false;
    }

    const std::string_view number_text = trim_view(text.substr(0, colon));
    const std::string type = lower(trim_view(text.substr(colon + 1, equals - colon - 1)));
    const std::string_view value_text = text.substr(equals + 1);

    if (number_text.empty()) {
        error = "missing field number";
        return false;
    }
    if (type.empty()) {
        error = "missing field type";
        return false;
    }
    if (!supported_type(type)) {
        error = "unsupported field type: " + type;
        return false;
    }

    std::uint64_t number = 0;
    const auto [ptr, ec] =
        std::from_chars(number_text.data(), number_text.data() + number_text.size(), number);
    if (ec != std::errc{} || ptr != number_text.data() + number_text.size()) {
        error = "invalid field number: " + std::string(number_text);
        return false;
    }
    if (number == 0 || number > 536870911u) {
        error = "field number must be in range 1..536870911";
        return false;
    }

    field.number = static_cast<std::uint32_t>(number);
    field.type = type;
    field.value = std::string(value_text);
    return true;
}

bool build_greeting_payload(const GreetingConfig& config, std::size_t max_payload,
                            std::vector<std::uint8_t>& payload, std::string& error) {
    payload.clear();
    error.clear();

    const int sources = (config.file.empty() ? 0 : 1) + (config.hex.empty() ? 0 : 1) +
                        (config.fields.empty() ? 0 : 1);
    if (sources == 0) {
        error = "no greeting source configured";
        return false;
    }
    if (sources != 1) {
        error = "use exactly one of --greeting-file, --greeting-hex or --greeting-field";
        return false;
    }

    if (!config.file.empty()) {
        if (!read_file(config.file, max_payload, payload, error)) {
            return false;
        }
    } else if (!config.hex.empty()) {
        if (!parse_hex_bytes(config.hex, payload, error)) {
            return false;
        }
        if (payload.size() > max_payload) {
            error = "greeting payload exceeds maximum payload size";
            return false;
        }
    } else {
        for (const std::string& field_text : config.fields) {
            GreetingField field;
            if (!parse_greeting_field(field_text, field, error)) {
                return false;
            }
            if (!encode_field(field, payload, error)) {
                return false;
            }
            if (payload.size() > max_payload) {
                error = "greeting payload exceeds maximum payload size";
                return false;
            }
        }
    }

    if (payload.empty()) {
        error = "greeting payload is empty";
        return false;
    }
    return true;
}

bool resolve_greeting_framing(const GreetingConfig& config, const FramingSpec& learned,
                              FramingSpec& framing, std::string& error) {
    error.clear();

    if (!config.framing) {
        error = "--greeting-framing is required";
        return false;
    }

    if (config.framing->kind != FramingKind::Auto) {
        framing = *config.framing;
    } else {
        if (learned.kind == FramingKind::Auto) {
            error = "learned framing is not available yet";
            return false;
        }
        if (learned.kind == FramingKind::Raw) {
            error = "cannot derive a greeting framing from the raw fallback";
            return false;
        }
        framing = learned;
    }

    framing.type_value = config.message_type;
    return true;
}

bool encode_greeting_wire(const GreetingConfig& config, const FramingSpec& framing,
                          ByteSpan payload, std::vector<std::uint8_t>& wire,
                          std::string& error) {
    wire.clear();
    error.clear();

    if (!config.enabled()) {
        error = "no greeting configured";
        return false;
    }
    if (framing.kind == FramingKind::Auto) {
        error = "greeting framing must be resolved before encoding";
        return false;
    }

    FramingSpec spec = framing;
    spec.type_value = config.message_type;
    return encode_frame(spec, payload, wire, error);
}

bool build_greeting_wire(const GreetingConfig& config, std::size_t max_payload,
                         std::vector<std::uint8_t>& wire, std::string& error) {
    wire.clear();
    error.clear();

    FramingSpec framing;
    if (!resolve_greeting_framing(config, FramingSpec{}, framing, error)) {
        return false;
    }

    std::vector<std::uint8_t> payload;
    if (!build_greeting_payload(config, max_payload, payload, error)) {
        return false;
    }

    return encode_greeting_wire(config, framing,
                                ByteSpan(payload.data(), payload.size()), wire, error);
}

bool encode_greeting_wire(std::uint64_t message_type, const FramingSpec& framing,
                         ByteSpan payload, std::vector<std::uint8_t>& wire,
                         std::string& error) {
    wire.clear();
    error.clear();

    if (framing.kind == FramingKind::Auto) {
        error = "greeting framing must be resolved before encoding";
        return false;
    }

    FramingSpec spec = framing;
    spec.type_value = message_type;
    return encode_frame(spec, payload, wire, error);
}

} // namespace pr
