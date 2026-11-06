#pragma once

#include "decode/message_decoder.hpp"
#include "emit/message_spool.hpp"
#include "infer/type_resolver.hpp"

#include <cstdint>
#include <string>

namespace pr {

struct JsonOptions {
    // 0 means unlimited.
    std::size_t hexdump_bytes = 0;
};

// Replays the spool and writes one JSON object per line. The output file is
// replaced atomically on success.
bool write_messages_json(const std::string& path, MessageSpool& spool,
                         const ResolvedModel& model, const JsonOptions& options,
                         const ParseLimits& limits, std::uint64_t& messages_written,
                         std::string& error);

} // namespace pr
