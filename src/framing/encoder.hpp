#pragma once

#include "framing/spec.hpp"
#include "wire/reader.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace pr {

// Encodes one payload with the given framing. The spec must be concrete:
// FramingKind::Auto is rejected. The output is cleared before encoding.
bool encode_frame(const FramingSpec& spec, ByteSpan payload, std::vector<std::uint8_t>& out,
                  std::string& error);

} // namespace pr
