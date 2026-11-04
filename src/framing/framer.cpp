#include "framing/framer.hpp"

#include <algorithm>
#include <cstring>

namespace pr {
namespace {

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

std::unique_ptr<Framer> make_framer(const FramingSpec& spec, std::size_t max_frame) {
    switch (spec.kind) {
    case FramingKind::Length:
        return std::make_unique<LengthFramer>(spec, max_frame);
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
