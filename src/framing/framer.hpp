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

std::unique_ptr<Framer> make_framer(const FramingSpec& spec, std::size_t max_frame);

} // namespace pr
