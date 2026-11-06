#pragma once

#include "wire/reader.hpp"

#include <cstdint>
#include <optional>

namespace pr {

// A successfully parsed message (or raw field in raw mode) that can be
// persisted for later decoding with the final inferred schema.
struct MessageRecord {
    std::uint64_t session = 0;
    std::uint64_t index = 0;
    std::optional<std::uint64_t> frame_type;
    ByteSpan payload;
};

class MessageSink {
public:
    virtual ~MessageSink() = default;
    virtual void record(const MessageRecord& message) = 0;
};

} // namespace pr
