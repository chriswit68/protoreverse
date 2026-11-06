#pragma once

#include "framing/spec.hpp"
#include "wire/reader.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pr {

struct GreetingConfig {
    std::string file;
    std::string hex;
    std::vector<std::string> fields;
    std::optional<FramingSpec> framing;

    // Message type written by type-prefixed framings. Ignored by framings
    // without a type field.
    std::uint64_t message_type = 0;
    bool message_type_set = false;

    bool enabled() const { return !file.empty() || !hex.empty() || !fields.empty(); }
};

struct GreetingField {
    std::uint32_t number = 0;
    std::string type;
    std::string value;
};

// Parses "<field-number>:<type>=<value>", for example "4:int32=2".
bool parse_greeting_field(std::string_view text, GreetingField& field, std::string& error);

// Builds the unframed protobuf payload from the configured source.
bool build_greeting_payload(const GreetingConfig& config, std::size_t max_payload,
                            std::vector<std::uint8_t>& payload, std::string& error);

// Resolves the framing used for the greeting. An explicit greeting framing is
// used as configured. Auto derives the framing from the learned incoming
// framing; a raw fallback is rejected because it cannot describe a reply.
bool resolve_greeting_framing(const GreetingConfig& config, const FramingSpec& learned,
                              FramingSpec& framing, std::string& error);

// Encodes an already prepared payload with a concrete/resolved framing, applying
// the configured greeting message type.
bool encode_greeting_wire(const GreetingConfig& config, const FramingSpec& framing,
                          ByteSpan payload, std::vector<std::uint8_t>& wire,
                          std::string& error);

// Builds the complete wire representation using an explicit greeting framing.
// Auto framings cannot be built before the learned framing is known.
bool build_greeting_wire(const GreetingConfig& config, std::size_t max_payload,
                         std::vector<std::uint8_t>& wire, std::string& error);

// Encodes an explicit payload as a greeting with a concrete framing and a
// concrete message type. Used for replying with the first client message.
bool encode_greeting_wire(std::uint64_t message_type, const FramingSpec& framing,
                         ByteSpan payload, std::vector<std::uint8_t>& wire,
                         std::string& error);

} // namespace pr
