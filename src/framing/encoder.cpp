#include "framing/encoder.hpp"

#include "wire/writer.hpp"

#include <limits>
#include <string>

namespace pr {
namespace {

bool append_fixed_value(std::uint64_t value, std::uint8_t width, bool big_endian,
                        const char* label, std::vector<std::uint8_t>& out,
                        std::string& error) {
    if (width != 1 && width != 2 && width != 4 && width != 8) {
        error = std::string(label) + " width must be 1, 2, 4 or 8";
        return false;
    }

    const unsigned bits = static_cast<unsigned>(width) * 8;
    const std::uint64_t maximum =
        bits == 64 ? std::numeric_limits<std::uint64_t>::max()
                   : (std::uint64_t{1} << bits) - 1;
    if (value > maximum) {
        error = std::string(label) + " does not fit into the configured width";
        return false;
    }

    for (std::uint8_t i = 0; i < width; ++i) {
        const unsigned shift =
            big_endian ? static_cast<unsigned>((static_cast<unsigned>(width) - 1 - i) * 8)
                       : static_cast<unsigned>(i * 8);
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
    }
    return true;
}

bool append_length_prefix(const FramingSpec& spec, std::uint64_t value,
                          std::vector<std::uint8_t>& out, std::string& error) {
    return append_fixed_value(value, spec.width, spec.big_endian, "length prefix", out,
                              error);
}

} // namespace

bool encode_frame(const FramingSpec& spec, ByteSpan payload, std::vector<std::uint8_t>& out,
                  std::string& error) {
    out.clear();
    error.clear();

    switch (spec.kind) {
    case FramingKind::Auto:
        error = "auto framing cannot be encoded; specify a concrete framing";
        return false;

    case FramingKind::Raw:
        out.insert(out.end(), payload.begin(), payload.end());
        return true;

    case FramingKind::Length: {
        const std::uint64_t value =
            static_cast<std::uint64_t>(payload.size()) +
            (spec.includes_header ? static_cast<std::uint64_t>(spec.width) : 0);
        if (!append_length_prefix(spec, value, out, error)) {
            return false;
        }
        out.insert(out.end(), payload.begin(), payload.end());
        return true;
    }

    case FramingKind::TypedLength: {
        const std::size_t header =
            static_cast<std::size_t>(spec.type_width) + spec.width;
        const std::uint64_t value =
            static_cast<std::uint64_t>(payload.size()) +
            (spec.includes_header ? static_cast<std::uint64_t>(header) : 0);
        if (!append_fixed_value(spec.type_value, spec.type_width, spec.type_big_endian,
                                "type prefix", out, error)) {
            return false;
        }
        if (!append_length_prefix(spec, value, out, error)) {
            return false;
        }
        out.insert(out.end(), payload.begin(), payload.end());
        return true;
    }

    case FramingKind::Varint:
        append_varint(out, payload.size());
        out.insert(out.end(), payload.begin(), payload.end());
        return true;

    case FramingKind::Grpc:
        if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
            error = "gRPC payload exceeds 32-bit length limit";
            return false;
        }
        out.push_back(0);
        out.push_back(static_cast<std::uint8_t>((payload.size() >> 24) & 0xff));
        out.push_back(static_cast<std::uint8_t>((payload.size() >> 16) & 0xff));
        out.push_back(static_cast<std::uint8_t>((payload.size() >> 8) & 0xff));
        out.push_back(static_cast<std::uint8_t>(payload.size() & 0xff));
        out.insert(out.end(), payload.begin(), payload.end());
        return true;

    case FramingKind::Newline:
        out.insert(out.end(), payload.begin(), payload.end());
        out.push_back('\n');
        return true;

    case FramingKind::Nul:
        out.insert(out.end(), payload.begin(), payload.end());
        out.push_back('\0');
        return true;

    case FramingKind::Fixed:
        if (payload.size() != spec.fixed_size) {
            error = "payload size " + std::to_string(payload.size()) +
                    " does not match fixed frame size " +
                    std::to_string(spec.fixed_size);
            return false;
        }
        out.insert(out.end(), payload.begin(), payload.end());
        return true;
    }

    error = "unknown framing";
    return false;
}

} // namespace pr
