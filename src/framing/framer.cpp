#include "framing/framer.hpp"

#include <algorithm>
#include <cstring>

namespace pr {
namespace {

std::uint64_t decode_fixed_field(ByteSpan data, std::uint8_t width, bool big_endian) {
    if (width == 1) {
        return data[0];
    }
    if (width == 2) {
        const std::uint16_t a = data[0];
        const std::uint16_t b = data[1];
        return big_endian ? static_cast<std::uint16_t>((a << 8) | b)
                          : static_cast<std::uint16_t>((b << 8) | a);
    }
    if (width == 4) {
        return big_endian ? decode_fixed32_be(data.first(4)) : decode_fixed32_le(data.first(4));
    }
    return big_endian ? decode_fixed64_be(data.first(8)) : decode_fixed64_le(data.first(8));
}

class LengthFramer final : public Framer {
public:
    LengthFramer(FramingSpec spec, std::size_t max_frame)
        : spec_(spec), max_frame_(max_frame) {}

    FrameStatus next(ByteSpan buffer, std::size_t& consumed, ByteSpan& frame,
                     std::string& error) override {
        consumed = 0;
        if (buffer.size() < spec_.width) {
            return FrameStatus::NeedMore;
        }

        std::uint64_t length = 0;
        if (spec_.width == 1) {
            length = buffer[0];
        } else if (spec_.width == 2) {
            const std::uint16_t a = buffer[0];
            const std::uint16_t b = buffer[1];
            length = spec_.big_endian ? static_cast<std::uint16_t>((a << 8) | b)
                                      : static_cast<std::uint16_t>((b << 8) | a);
        } else if (spec_.width == 4) {
            length = spec_.big_endian ? decode_fixed32_be(buffer.first(4))
                                      : decode_fixed32_le(buffer.first(4));
        } else {
            length = spec_.big_endian ? decode_fixed64_be(buffer.first(8))
                                      : decode_fixed64_le(buffer.first(8));
        }

        if (spec_.includes_header) {
            if (length < spec_.width) {
                error = "length prefix is smaller than the header";
                return FrameStatus::Error;
            }
            length -= spec_.width;
        }

        if (length > max_frame_) {
            error = "frame length exceeds maximum";
            return FrameStatus::Error;
        }

        const std::size_t total = static_cast<std::size_t>(length) + spec_.width;
        if (buffer.size() < total) {
            return FrameStatus::NeedMore;
        }

        frame = buffer.subspan(spec_.width, static_cast<std::size_t>(length));
        consumed = total;
        return FrameStatus::Complete;
    }

    FramingSpec spec() const override { return spec_; }

private:
    FramingSpec spec_;
    std::size_t max_frame_;
};

class TypedLengthFramer final : public Framer {
public:
    TypedLengthFramer(FramingSpec spec, std::size_t max_frame)
        : spec_(spec), max_frame_(max_frame) {}

    FrameStatus next(ByteSpan buffer, std::size_t& consumed, ByteSpan& frame,
                     std::string& error) override {
        consumed = 0;
        const std::size_t header = header_size();
        if (buffer.size() < header) {
            return FrameStatus::NeedMore;
        }

        const std::uint64_t encoded =
            decode_fixed_field(buffer.subspan(spec_.type_width, spec_.width), spec_.width,
                               spec_.big_endian);

        std::uint64_t length = encoded;
        if (spec_.includes_header) {
            if (encoded < header) {
                error = "length prefix is smaller than the typed header";
                return FrameStatus::Error;
            }
            length = encoded - header;
        }

        if (length > max_frame_) {
            error = "frame length exceeds maximum";
            return FrameStatus::Error;
        }

        const std::size_t total = header + static_cast<std::size_t>(length);
        if (buffer.size() < total) {
            return FrameStatus::NeedMore;
        }

        frame = buffer.subspan(header, static_cast<std::size_t>(length));
        consumed = total;
        last_frame_type_ = decode_fixed_field(buffer, spec_.type_width,
                                              spec_.type_big_endian);
        return FrameStatus::Complete;
    }

    FramingSpec spec() const override { return spec_; }

    std::optional<std::uint64_t> last_frame_type() const override {
        return last_frame_type_;
    }

private:
    std::size_t header_size() const {
        return static_cast<std::size_t>(spec_.type_width) + spec_.width;
    }

    FramingSpec spec_;
    std::size_t max_frame_;
    std::optional<std::uint64_t> last_frame_type_;
};

class VarintFramer final : public Framer {
public:
    explicit VarintFramer(std::size_t max_frame) : max_frame_(max_frame) {}

    FrameStatus next(ByteSpan buffer, std::size_t& consumed, ByteSpan& frame,
                     std::string& error) override {
        consumed = 0;
        WireReader reader(buffer);
        std::uint64_t length = 0;
        const ReadResult result = reader.read_varint(length);
        if (result == ReadResult::NeedMore) {
            return FrameStatus::NeedMore;
        }
        if (result != ReadResult::Ok) {
            error = "invalid varint length prefix";
            return FrameStatus::Error;
        }
        if (length > max_frame_) {
            error = "frame length exceeds maximum";
            return FrameStatus::Error;
        }

        const std::size_t prefix = reader.position();
        if (buffer.size() < prefix + static_cast<std::size_t>(length)) {
            return FrameStatus::NeedMore;
        }

        frame = buffer.subspan(prefix, static_cast<std::size_t>(length));
        consumed = prefix + static_cast<std::size_t>(length);
        return FrameStatus::Complete;
    }

    FramingSpec spec() const override {
        FramingSpec spec;
        spec.kind = FramingKind::Varint;
        return spec;
    }

private:
    std::size_t max_frame_;
};

class GrpcFramer final : public Framer {
public:
    explicit GrpcFramer(std::size_t max_frame) : max_frame_(max_frame) {}

    FrameStatus next(ByteSpan buffer, std::size_t& consumed, ByteSpan& frame,
                     std::string& error) override {
        consumed = 0;
        if (buffer.size() < 5) {
            return FrameStatus::NeedMore;
        }
        if (buffer[0] != 0) {
            error = "compressed gRPC frames are not supported";
            return FrameStatus::Error;
        }
        const std::uint32_t length = decode_fixed32_be(buffer.subspan(1, 4));
        if (length > max_frame_) {
            error = "gRPC frame length exceeds maximum";
            return FrameStatus::Error;
        }
        if (buffer.size() < 5 + static_cast<std::size_t>(length)) {
            return FrameStatus::NeedMore;
        }
        frame = buffer.subspan(5, length);
        consumed = 5 + static_cast<std::size_t>(length);
        return FrameStatus::Complete;
    }

    FramingSpec spec() const override {
        FramingSpec spec;
        spec.kind = FramingKind::Grpc;
        return spec;
    }

private:
    std::size_t max_frame_;
};

class DelimiterFramer final : public Framer {
public:
    DelimiterFramer(std::uint8_t delimiter, FramingKind kind, std::size_t max_frame)
        : delimiter_(delimiter), kind_(kind), max_frame_(max_frame) {}

    FrameStatus next(ByteSpan buffer, std::size_t& consumed, ByteSpan& frame,
                     std::string& error) override {
        consumed = 0;
        const auto it = std::find(buffer.begin(), buffer.end(), delimiter_);
        if (it == buffer.end()) {
            if (buffer.size() > max_frame_) {
                error = "delimited frame exceeds maximum";
                return FrameStatus::Error;
            }
            return FrameStatus::NeedMore;
        }

        const std::size_t length = static_cast<std::size_t>(std::distance(buffer.begin(), it));
        if (length == 0) {
            error = "empty delimited frame";
            return FrameStatus::Error;
        }
        if (length > max_frame_) {
            error = "delimited frame exceeds maximum";
            return FrameStatus::Error;
        }

        frame = buffer.first(length);
        consumed = length + 1;
        return FrameStatus::Complete;
    }

    FramingSpec spec() const override {
        FramingSpec spec;
        spec.kind = kind_;
        return spec;
    }

private:
    std::uint8_t delimiter_;
    FramingKind kind_;
    std::size_t max_frame_;
};

class FixedSizeFramer final : public Framer {
public:
    FixedSizeFramer(std::size_t size, std::size_t max_frame)
        : size_(size), max_frame_(max_frame) {}

    FrameStatus next(ByteSpan buffer, std::size_t& consumed, ByteSpan& frame,
                     std::string& error) override {
        consumed = 0;
        if (size_ > max_frame_) {
            error = "fixed frame size exceeds maximum";
            return FrameStatus::Error;
        }
        if (buffer.size() < size_) {
            return FrameStatus::NeedMore;
        }
        frame = buffer.first(size_);
        consumed = size_;
        return FrameStatus::Complete;
    }

    FramingSpec spec() const override {
        FramingSpec spec;
        spec.kind = FramingKind::Fixed;
        spec.fixed_size = size_;
        return spec;
    }

private:
    std::size_t size_;
    std::size_t max_frame_;
};

} // namespace

FrameProbe probe_frame(const FramingSpec& spec, ByteSpan buffer, std::size_t max_frame) {
    FrameProbe probe;

    switch (spec.kind) {
    case FramingKind::Length: {
        if (buffer.size() < spec.width) {
            return probe;
        }
        probe.header_ready = true;
        probe.header_size = spec.width;

        std::uint64_t encoded = 0;
        if (spec.width == 1) {
            encoded = buffer[0];
        } else if (spec.width == 2) {
            const std::uint16_t a = buffer[0];
            const std::uint16_t b = buffer[1];
            encoded = spec.big_endian ? static_cast<std::uint16_t>((a << 8) | b)
                                      : static_cast<std::uint16_t>((b << 8) | a);
        } else if (spec.width == 4) {
            encoded = spec.big_endian ? decode_fixed32_be(buffer.first(4))
                                      : decode_fixed32_le(buffer.first(4));
        } else {
            encoded = spec.big_endian ? decode_fixed64_be(buffer.first(8))
                                      : decode_fixed64_le(buffer.first(8));
        }

        std::uint64_t payload_length = encoded;
        if (spec.includes_header) {
            if (encoded < spec.width) {
                probe.error = "length prefix is smaller than the header";
                return probe;
            }
            payload_length = encoded - spec.width;
        }

        if (payload_length > max_frame) {
            probe.error = "frame length exceeds maximum";
            return probe;
        }

        probe.declared_length = payload_length;
        probe.total_size = static_cast<std::size_t>(payload_length) + spec.width;
        probe.valid = true;
        return probe;
    }

    case FramingKind::TypedLength: {
        const std::size_t header =
            static_cast<std::size_t>(spec.type_width) + spec.width;
        if (buffer.size() < header) {
            return probe;
        }
        probe.header_ready = true;
        probe.header_size = header;

        const std::uint64_t encoded =
            decode_fixed_field(buffer.subspan(spec.type_width, spec.width), spec.width,
                               spec.big_endian);

        std::uint64_t payload_length = encoded;
        if (spec.includes_header) {
            if (encoded < header) {
                probe.error = "length prefix is smaller than the typed header";
                return probe;
            }
            payload_length = encoded - header;
        }

        if (payload_length > max_frame) {
            probe.error = "frame length exceeds maximum";
            return probe;
        }

        probe.declared_length = payload_length;
        probe.total_size = header + static_cast<std::size_t>(payload_length);
        probe.valid = true;
        return probe;
    }

    case FramingKind::Varint: {
        WireReader reader(buffer);
        std::uint64_t length = 0;
        const ReadResult result = reader.read_varint(length);
        if (result == ReadResult::NeedMore) {
            return probe;
        }
        probe.header_ready = true;
        probe.header_size = reader.position();
        if (result != ReadResult::Ok) {
            probe.error = "invalid varint length prefix";
            return probe;
        }
        if (length > max_frame) {
            probe.error = "frame length exceeds maximum";
            return probe;
        }
        probe.declared_length = length;
        probe.total_size = probe.header_size + static_cast<std::size_t>(length);
        probe.valid = true;
        return probe;
    }

    case FramingKind::Grpc: {
        if (buffer.size() < 5) {
            return probe;
        }
        probe.header_ready = true;
        probe.header_size = 5;
        if (buffer[0] != 0) {
            probe.error = "compressed gRPC frames are not supported";
            return probe;
        }
        const std::uint32_t length = decode_fixed32_be(buffer.subspan(1, 4));
        if (length > max_frame) {
            probe.error = "gRPC frame length exceeds maximum";
            return probe;
        }
        probe.declared_length = length;
        probe.total_size = 5 + static_cast<std::size_t>(length);
        probe.valid = true;
        return probe;
    }

    case FramingKind::Newline:
    case FramingKind::Nul: {
        const std::uint8_t delimiter =
            spec.kind == FramingKind::Newline ? static_cast<std::uint8_t>('\n')
                                              : static_cast<std::uint8_t>('\0');
        const auto it = std::find(buffer.begin(), buffer.end(), delimiter);
        if (it == buffer.end()) {
            if (buffer.size() > max_frame) {
                probe.error = "delimited frame exceeds maximum";
            }
            return probe;
        }
        probe.header_ready = true;
        probe.header_size = 0;
        const std::size_t length = static_cast<std::size_t>(std::distance(buffer.begin(), it));
        if (length == 0) {
            probe.error = "empty delimited frame";
            return probe;
        }
        if (length > max_frame) {
            probe.error = "delimited frame exceeds maximum";
            return probe;
        }
        probe.declared_length = length;
        probe.total_size = length + 1;
        probe.valid = true;
        return probe;
    }

    case FramingKind::Fixed: {
        if (spec.fixed_size == 0) {
            probe.error = "invalid fixed frame size";
            return probe;
        }
        if (spec.fixed_size > max_frame) {
            probe.error = "fixed frame size exceeds maximum";
            return probe;
        }
        probe.header_ready = buffer.size() >= spec.fixed_size;
        probe.header_size = 0;
        probe.declared_length = spec.fixed_size;
        probe.total_size = spec.fixed_size;
        probe.valid = probe.header_ready;
        return probe;
    }

    case FramingKind::Auto:
    case FramingKind::Raw:
        return probe;
    }

    return probe;
}

std::unique_ptr<Framer> make_framer(const FramingSpec& spec, std::size_t max_frame) {
    switch (spec.kind) {
    case FramingKind::Length:
        return std::make_unique<LengthFramer>(spec, max_frame);
    case FramingKind::TypedLength:
        return std::make_unique<TypedLengthFramer>(spec, max_frame);
    case FramingKind::Varint:
        return std::make_unique<VarintFramer>(max_frame);
    case FramingKind::Grpc:
        return std::make_unique<GrpcFramer>(max_frame);
    case FramingKind::Newline:
        return std::make_unique<DelimiterFramer>('\n', FramingKind::Newline, max_frame);
    case FramingKind::Nul:
        return std::make_unique<DelimiterFramer>('\0', FramingKind::Nul, max_frame);
    case FramingKind::Fixed:
        return std::make_unique<FixedSizeFramer>(spec.fixed_size, max_frame);
    case FramingKind::Auto:
    case FramingKind::Raw:
        return nullptr;
    }
    return nullptr;
}

} // namespace pr
