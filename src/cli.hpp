#pragma once

#include "framing/spec.hpp"
#include "greeting/greeting.hpp"
#include "log.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <string>

namespace pr {

enum class RunMode {
    Serve,
    Analyze,
};

struct Config {
    RunMode mode = RunMode::Serve;

    // Networking
    std::string bind_address = "0.0.0.0";
    std::uint16_t port = 9000;

    // Optional server-initiated greeting (serve mode only)
    GreetingConfig greeting;

    // Input/output
    std::string input_path;
    bool input_stdin = false;
    std::string output_path = "schema.proto";
    bool learning_only = false;

    // Framing and limits
    FramingSpec framing;
    std::size_t start_offset = 0;
    std::size_t learning_bytes = 64 * 1024;
    std::size_t learning_frames = 32;
    std::chrono::milliseconds learning_timeout{2000};
    std::size_t max_frame = 16 * 1024 * 1024;
    std::uint64_t max_messages = 0;
    std::chrono::milliseconds run_for{0};
    std::chrono::milliseconds idle_timeout{60000};
    std::uint64_t autosave = 1000;
    bool once = false;

    // Proto output
    bool proto2 = false;
    std::string package = "inferred";
    std::string root_name = "ClientMessage";

    // Diagnostics
    LogLevel level = LogLevel::Info;
    bool hexdump = false;
    std::size_t hexdump_bytes = 64;
    bool no_samples = false;
    bool strict = false;

    // Resource limits
    std::size_t max_learning_memory = 4 * 1024 * 1024;
};

class CliError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

Config parse_args(int argc, char** argv);
void print_usage(std::ostream& out);

} // namespace pr
