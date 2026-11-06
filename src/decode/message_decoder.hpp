#pragma once

#include "infer/type_resolver.hpp"
#include "wire/reader.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace pr {

struct DecodeSchema {
    const ResolvedMessage* root = nullptr;
    std::map<std::string, const ResolvedMessage*> messages;
};

DecodeSchema make_decode_schema(const ResolvedModel& model);

struct DecodeOptions {
    // 0 means unlimited.
    std::size_t hexdump_bytes = 0;
};

struct MessageMetadata {
    std::uint64_t session = 0;
    std::uint64_t index = 0;
    std::optional<std::uint64_t> frame_type;
};

// Renders exactly one JSON object. A payload that cannot be parsed still
// produces an object containing an "error" field instead of failing.
void render_message_json(ByteSpan payload, const DecodeSchema& schema,
                         const MessageMetadata& metadata, const DecodeOptions& options,
                         const ParseLimits& limits, std::string& out);

} // namespace pr
