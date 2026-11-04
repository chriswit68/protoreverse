#pragma once

#include "framing/spec.hpp"

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

// Builds the complete wire representation (payload plus framing).
bool build_greeting_wire(const GreetingConfig& config, std::size_t max_payload,
                         std::vector<std::uint8_t>& wire, std::string& error);

} // namespace pr
