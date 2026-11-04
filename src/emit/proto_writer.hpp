#pragma once

#include "infer/type_resolver.hpp"

#include <cstdint>
#include <string>

namespace pr {

struct ProtoOptions {
    bool proto2 = false;
    std::string package = "inferred";
    std::string framing_label = "auto";
    double framing_confidence = 0.0;
    std::uint64_t sessions = 0;
    std::uint64_t bytes = 0;
    std::uint64_t frames = 0;
};

std::string render_proto(const ResolvedModel& model, const ProtoOptions& options);
bool write_proto_file(const std::string& path, const std::string& content, std::string& error);

} // namespace pr
