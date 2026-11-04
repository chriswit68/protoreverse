#pragma once

#include "infer/schema.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace pr {

struct ResolvedField {
    std::uint32_t number = 0;
    std::string name;
    std::string type;
    bool repeated = false;
    bool packed = false;
    bool valid = true;
    double confidence = 0.0;
    std::string comment;
};

struct ResolvedMessage {
    std::string name;
    std::vector<ResolvedField> fields;
    std::string comment;
};

struct ResolvedModel {
    std::string root_name;
    std::vector<ResolvedMessage> messages;
    bool raw_mode = false;
    std::uint64_t message_count = 0;
    std::uint64_t field_count = 0;
};

struct ResolveOptions {
    std::string root_name = "ClientMessage";
    bool proto2 = false;
    bool raw_mode = false;
};

ResolvedModel resolve_schema(const MessageSchema& root, const ResolveOptions& options);

} // namespace pr
