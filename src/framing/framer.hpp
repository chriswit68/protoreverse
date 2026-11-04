#pragma once

#include "framing/spec.hpp"
#include "wire/reader.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>

namespace pr {

enum class FrameStatus {
    Complete,
    NeedMore,
    Error,
};

class Framer {
public:
    virtual ~Framer() = default;

    virtual FrameStatus next(ByteSpan buffer, std::size_t& consumed, ByteSpan& frame,
                             std::string& error) = 0;
    virtual FramingSpec spec() const = 0;
};

// Non-consuming inspection of the next frame header. This is used by the
// framing learner to evaluate a candidate before a complete frame is
// available. header_ready=false means that more bytes are needed to read the
// header; valid=false with a non-empty error means the candidate is malformed.
struct FrameProbe {
    bool header_ready = false;
    bool valid = false;
    std::uint64_t declared_length = 0;
    std::size_t header_size = 0;
    std::size_t total_size = 0;
    std::string error;
};

FrameProbe probe_frame(const FramingSpec& spec, ByteSpan buffer, std::size_t max_frame);

std::unique_ptr<Framer> make_framer(const FramingSpec& spec, std::size_t max_frame);

} // namespace pr
