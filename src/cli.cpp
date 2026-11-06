#include "cli.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <vector>

namespace pr {
namespace {

[[noreturn]] void fail(const std::string& message) {
    throw CliError(message);
}

std::string lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::uint64_t parse_u64(std::string_view text, const std::string& option) {
    std::uint64_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        fail("invalid value for " + option + ": " + std::string(text));
    }
    return value;
}

std::size_t parse_size(std::string_view text, const std::string& option) {
    const std::uint64_t value = parse_u64(text, option);
    if (value > std::numeric_limits<std::size_t>::max()) {
        fail("value too large for " + option);
    }
    return static_cast<std::size_t>(value);
}

std::size_t parse_byte_size(std::string_view text, const std::string& option) {
    const std::string cleaned = lower(trim(text));
    if (cleaned.empty()) {
        fail("empty value for " + option);
    }

    std::size_t pos = 0;
    while (pos < cleaned.size() && std::isdigit(static_cast<unsigned char>(cleaned[pos]))) {
        ++pos;
    }
    if (pos == 0) {
        fail("invalid byte size for " + option + ": " + std::string(text));
    }

    const std::uint64_t base = parse_u64(std::string_view(cleaned).substr(0, pos), option);
    std::string suffix = cleaned.substr(pos);
    std::uint64_t multiplier = 1;
    if (suffix.empty() || suffix == "b") {
        multiplier = 1;
    } else if (suffix == "k" || suffix == "kb" || suffix == "kib") {
        multiplier = 1024;
    } else if (suffix == "m" || suffix == "mb" || suffix == "mib") {
        multiplier = 1024 * 1024;
    } else if (suffix == "g" || suffix == "gb" || suffix == "gib") {
        multiplier = 1024ull * 1024ull * 1024ull;
    } else {
        fail("invalid byte size suffix for " + option + ": " + suffix);
    }

    if (base > std::numeric_limits<std::size_t>::max() / multiplier) {
        fail("byte size too large for " + option);
    }
    return static_cast<std::size_t>(base * multiplier);
}

std::chrono::milliseconds parse_duration(std::string_view text, const std::string& option) {
    const std::string cleaned = lower(trim(text));
    if (cleaned.empty()) {
        fail("empty duration for " + option);
    }

    std::size_t pos = 0;
    while (pos < cleaned.size() && std::isdigit(static_cast<unsigned char>(cleaned[pos]))) {
        ++pos;
    }
    if (pos == 0) {
        fail("invalid duration for " + option + ": " + std::string(text));
    }

    const std::uint64_t value =
        parse_u64(std::string_view(cleaned).substr(0, pos), option);
    const std::string suffix = cleaned.substr(pos);

    std::uint64_t multiplier = 1;
    if (suffix.empty() || suffix == "ms") {
        multiplier = 1;
    } else if (suffix == "s") {
        multiplier = 1000;
    } else if (suffix == "m") {
        multiplier = 60 * 1000;
    } else if (suffix == "h") {
        multiplier = 60 * 60 * 1000;
    } else {
        fail("invalid duration suffix for " + option + ": " + suffix);
    }

    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / multiplier) {
        fail("duration too large for " + option);
    }
    return std::chrono::milliseconds(static_cast<std::int64_t>(value * multiplier));
}

bool valid_identifier(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    if (!std::isalpha(static_cast<unsigned char>(text[0])) && text[0] != '_') {
        return false;
    }
    for (char c : text) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
            return false;
        }
    }
    return true;
}

bool valid_package(std::string_view text) {
    if (text.empty()) {
        return true;
    }
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t pos = text.find('.', start);
        const std::string_view part =
            pos == std::string_view::npos ? text.substr(start) : text.substr(start, pos - start);
        if (!valid_identifier(part)) {
            return false;
        }
        if (pos == std::string_view::npos) {
            break;
        }
        start = pos + 1;
    }
    return true;
}

struct Arg {
    std::string key;
    std::optional<std::string> value;
};

Arg split_arg(const std::string& raw) {
    const std::size_t pos = raw.find('=');
    if (pos == std::string::npos) {
        return {raw, std::nullopt};
    }
    return {raw.substr(0, pos), raw.substr(pos + 1)};
}

} // namespace

void print_usage(std::ostream& out) {
    out << R"(protoreverse - infer a .proto schema from protobuf byte streams

Usage:
  protoreverse serve   [options]
  protoreverse analyze --input <file> [options]
  protoreverse analyze --stdin        [options]

Modes:
  serve      Listen on TCP and analyze all incoming connections.
  analyze    Analyze a capture file or stdin once.

Networking:
  --bind <addr>              Bind address (default: 0.0.0.0)
  --port <port>              TCP port (default: 9000)

Server greeting:
  --greeting-file <file>     Read the greeting payload from a file
  --greeting-hex <hex>       Read the greeting payload from hex
  --greeting-field <n>:<t>=<v>
                             Add a field to the generated greeting payload
                             (repeatable; e.g. 4:int32=2)
  --greeting-framing <spec>  Greeting framing, or auto to use the learned framing
  --greeting-type <n>        Message type for a type-prefixed greeting framing
                             (default: 0)
  --reply-with-first-message
                             Reply to the client with the first client message
                             instead of a configured greeting payload
  --alive-message            Send a header-only keep-alive frame to the client
                             periodically (requires --greeting-framing auto or
                             --reply-with-first-message)
  --alive-interval <dur>     Alive keep-alive interval in milliseconds or a
                             duration such as 30s (default: 30s)
  --alive-message-type <n>
                             Message type in the first 4 header bytes of the
                             alive frame (requires --alive-message)

Input/output:
  --input <file>             Input file for analyze mode
  --stdin                    Read from stdin in analyze mode
  --output <file>            Output .proto file (default: schema.proto)
  --json-output <file>       Also write decoded messages as JSON Lines
  --json-hexdump-bytes <n>   Maximum bytes per binary value in JSON output
                             (default: 0 = unlimited)
  --save-first-message <file>
                             Write the first analyzed message to <file>
  --learning-only            Print framing candidates and exit

Framing:
  --framing <spec>           auto (default), raw, varint, grpc, newline, nul,
                             fixed:<n>, len:<1|2|4|8>:<be|le>[:incl|:payload],
                             type:<1|2|4|8>:<be|le>+len:<1|2|4|8>:<be|le>
                             [:incl|:payload] or type4+len4:<be|le>
                             Aliases: u8, u16be, u16le, u32be, u32le, u64be, u64le
  --start-offset <bytes>     Skip this many bytes at the start of each session
  --learning-bytes <size>    Learning sample size (default: 64KiB)
  --learning-frames <n>      Learning frame target (default: 32)
  --learning-timeout <dur>   Max learning time (default: 2s)
  --early-lock <mode>        balanced (default) or off
                             balanced selects as soon as prefix and first tag
                             provide strong evidence
  --max-frame <size>         Maximum frame size (default: 16MiB)

Runtime:
  --max-messages <n>         Finalize after n messages (0 = unlimited)
  --run-for <dur>            Stop after the given duration (serve mode)
  --idle-timeout <dur>       Close idle sessions (default: 60s)
  --autosave <n>             Write a snapshot every n messages (0 = off)
  --once                     Stop after the first session ends

Output format:
  --proto2                   Emit proto2 instead of proto3
  --package <name>           Package name (default: inferred)
  --root-name <name>         Root message name (default: ClientMessage)

Diagnostics:
  --log-level <level>        trace, debug, info, warn, error, off
  --hexdump                  Log the first bytes of each session
  --hexdump-first <n>        Number of bytes to dump (default: 64)
  --no-samples               Do not retain sample values for inference
  --strict                   Exit on framing or parse errors
  --help                     Show this help

Examples:
  protoreverse serve --port 9000 --output schema.proto
  protoreverse analyze --input capture.bin --output schema.proto
  protoreverse analyze --input capture.bin --framing len:4:be
)";
}

Config parse_args(int argc, char** argv) {
    if (argc < 2) {
        fail("missing mode (serve or analyze)");
    }

    Config config;

    const std::string mode = argv[1];
    if (mode == "--help" || mode == "-h" || mode == "help") {
        print_usage(std::cout);
        std::exit(0);
    }
    if (mode == "serve") {
        config.mode = RunMode::Serve;
    } else if (mode == "analyze") {
        config.mode = RunMode::Analyze;
    } else {
        fail("unknown mode: " + mode);
    }

    for (int i = 2; i < argc; ++i) {
        const std::string raw = argv[i];
        if (raw == "--help" || raw == "-h") {
            print_usage(std::cout);
            std::exit(0);
        }
        if (!raw.starts_with("--")) {
            fail("unexpected argument: " + raw);
        }

        Arg arg = split_arg(raw);
        auto value = [&]() -> std::string {
            if (arg.value) {
                return *arg.value;
            }
            if (i + 1 >= argc) {
                fail("missing value for " + arg.key);
            }
            return argv[++i];
        };

        if (arg.key == "--bind") {
            config.bind_address = value();
        } else if (arg.key == "--port") {
            const std::uint64_t port = parse_u64(value(), arg.key);
            if (port == 0 || port > 65535) {
                fail("port must be in range 1..65535");
            }
            config.port = static_cast<std::uint16_t>(port);
        } else if (arg.key == "--greeting-file") {
            if (!config.greeting.file.empty()) {
                fail("duplicate --greeting-file");
            }
            config.greeting.file = value();
            if (config.greeting.file.empty()) {
                fail("--greeting-file must not be empty");
            }
        } else if (arg.key == "--greeting-hex") {
            if (!config.greeting.hex.empty()) {
                fail("duplicate --greeting-hex");
            }
            config.greeting.hex = value();
            if (config.greeting.hex.empty()) {
                fail("--greeting-hex must not be empty");
            }
        } else if (arg.key == "--greeting-field") {
            const std::string text = value();
            GreetingField field;
            std::string error;
            if (!parse_greeting_field(text, field, error)) {
                fail("invalid --greeting-field: " + error);
            }
            config.greeting.fields.push_back(text);
        } else if (arg.key == "--greeting-framing") {
            if (config.greeting.framing) {
                fail("duplicate --greeting-framing");
            }
            std::string error;
            const auto spec = parse_framing(value(), error);
            if (!spec) {
                fail(error);
            }
            config.greeting.framing = *spec;
        } else if (arg.key == "--greeting-type") {
            config.greeting.message_type = parse_u64(value(), arg.key);
            config.greeting.message_type_set = true;
        } else if (arg.key == "--reply-with-first-message") {
            config.reply_with_first_message = true;
        } else if (arg.key == "--alive-message") {
            config.alive_message = true;
        } else if (arg.key == "--alive-interval") {
            config.alive_interval = parse_duration(value(), arg.key);
        } else if (arg.key == "--alive-message-type") {
            config.alive_message_type = parse_u64(value(), arg.key);
            config.alive_message_type_set = true;
        } else if (arg.key == "--input") {
            config.input_path = value();
        } else if (arg.key == "--stdin") {
            config.input_stdin = true;
        } else if (arg.key == "--output") {
            config.output_path = value();
        } else if (arg.key == "--json-output") {
            config.json_output_path = value();
            if (config.json_output_path.empty()) {
                fail("--json-output must not be empty");
            }
        } else if (arg.key == "--json-hexdump-bytes") {
            config.json_hexdump_bytes = parse_size(value(), arg.key);
        } else if (arg.key == "--save-first-message") {
            config.save_first_message_path = value();
            if (config.save_first_message_path.empty()) {
                fail("--save-first-message must not be empty");
            }
        } else if (arg.key == "--learning-only") {
            config.learning_only = true;
        } else if (arg.key == "--framing") {
            std::string error;
            const auto spec = parse_framing(value(), error);
            if (!spec) {
                fail(error);
            }
            config.framing = *spec;
        } else if (arg.key == "--start-offset") {
            config.start_offset = parse_size(value(), arg.key);
        } else if (arg.key == "--learning-bytes") {
            config.learning_bytes = parse_byte_size(value(), arg.key);
        } else if (arg.key == "--learning-frames") {
            config.learning_frames = parse_size(value(), arg.key);
        } else if (arg.key == "--learning-timeout") {
            config.learning_timeout = parse_duration(value(), arg.key);
        } else if (arg.key == "--early-lock") {
            const std::string mode = lower(value());
            if (mode == "balanced" || mode == "on") {
                config.early_lock = true;
            } else if (mode == "off") {
                config.early_lock = false;
            } else {
                fail("--early-lock must be balanced or off");
            }
        } else if (arg.key == "--max-frame") {
            config.max_frame = parse_byte_size(value(), arg.key);
        } else if (arg.key == "--max-messages") {
            config.max_messages = parse_u64(value(), arg.key);
        } else if (arg.key == "--run-for") {
            config.run_for = parse_duration(value(), arg.key);
        } else if (arg.key == "--idle-timeout") {
            config.idle_timeout = parse_duration(value(), arg.key);
        } else if (arg.key == "--autosave") {
            config.autosave = parse_u64(value(), arg.key);
        } else if (arg.key == "--once") {
            config.once = true;
        } else if (arg.key == "--proto2") {
            config.proto2 = true;
        } else if (arg.key == "--proto3") {
            config.proto2 = false;
        } else if (arg.key == "--package") {
            config.package = value();
        } else if (arg.key == "--root-name") {
            config.root_name = value();
        } else if (arg.key == "--log-level") {
            LogLevel level{};
            if (!parse_log_level(value(), level)) {
                fail("invalid log level");
            }
            config.level = level;
        } else if (arg.key == "--hexdump") {
            config.hexdump = true;
        } else if (arg.key == "--hexdump-first") {
            config.hexdump = true;
            config.hexdump_bytes = parse_size(value(), arg.key);
        } else if (arg.key == "--no-samples") {
            config.no_samples = true;
        } else if (arg.key == "--strict") {
            config.strict = true;
        } else {
            fail("unknown option: " + arg.key);
        }
    }

    if (config.learning_bytes == 0) {
        fail("--learning-bytes must be greater than zero");
    }
    if (config.learning_frames == 0) {
        fail("--learning-frames must be greater than zero");
    }
    if (config.max_frame == 0) {
        fail("--max-frame must be greater than zero");
    }
    if (!valid_identifier(config.root_name)) {
        fail("--root-name is not a valid protobuf identifier");
    }
    if (!valid_package(config.package)) {
        fail("--package is not a valid protobuf package name");
    }
    if (config.output_path.empty()) {
        fail("--output must not be empty");
    }
    if (config.json_output_path.empty() && config.json_hexdump_bytes != 0) {
        fail("--json-hexdump-bytes requires --json-output");
    }
    if (!config.json_output_path.empty()) {
        if (config.json_output_path == config.output_path) {
            fail("--json-output must differ from --output");
        }
        if (config.learning_only) {
            fail("--json-output cannot be combined with --learning-only");
        }
    }
    if (!config.save_first_message_path.empty() && config.learning_only) {
        fail("--save-first-message cannot be combined with --learning-only");
    }

    const bool greeting_enabled = config.greeting.enabled();
    if (config.reply_with_first_message) {
        // The greeting is derived from the first client message instead of a
        // configured greeting source, so only an automatic greeting framing is
        // meaningful here.
        if (config.mode != RunMode::Serve) {
            fail("--reply-with-first-message is only valid in serve mode");
        }
        if (!config.greeting.framing) {
            fail("--greeting-framing is required with --reply-with-first-message");
        }
        if (config.greeting.framing->kind != FramingKind::Auto) {
            fail("--reply-with-first-message requires --greeting-framing auto");
        }
    } else if (greeting_enabled) {
        const int sources = (config.greeting.file.empty() ? 0 : 1) +
                            (config.greeting.hex.empty() ? 0 : 1) +
                            (config.greeting.fields.empty() ? 0 : 1);
        if (sources != 1) {
            fail("use exactly one of --greeting-file, --greeting-hex or --greeting-field");
        }
        if (!config.greeting.framing) {
            fail("--greeting-framing is required when a greeting is configured");
        }
        if (config.mode != RunMode::Serve) {
            fail("greeting options are only valid in serve mode");
        }
        if (config.greeting.message_type_set &&
            config.greeting.framing->kind != FramingKind::TypedLength &&
            config.greeting.framing->kind != FramingKind::Auto) {
            fail("--greeting-type requires a type-prefixed or auto --greeting-framing");
        }
    } else if (config.greeting.framing) {
        fail("--greeting-framing requires a greeting source");
    } else if (config.greeting.message_type_set) {
        fail("--greeting-type requires a greeting source");
    }

    if (config.alive_message) {
        if (config.mode != RunMode::Serve) {
            fail("--alive-message is only valid in serve mode");
        }
        const bool learned_framing =
            config.reply_with_first_message ||
            (config.greeting.enabled() &&
             config.greeting.framing &&
             config.greeting.framing->kind == FramingKind::Auto);
        if (!learned_framing) {
            fail("--alive-message requires --greeting-framing auto or --reply-with-first-message");
        }
        if (config.alive_interval.count() <= 0) {
            fail("--alive-interval must be positive");
        }
    }
    if (config.alive_message_type_set && !config.alive_message) {
        fail("--alive-message-type requires --alive-message");
    }

    if (config.mode == RunMode::Analyze) {
        if (config.input_path.empty() && !config.input_stdin) {
            fail("analyze mode requires --input <file> or --stdin");
        }
        if (!config.input_path.empty() && config.input_stdin) {
            fail("use either --input or --stdin, not both");
        }
    } else {
        if (!config.input_path.empty() || config.input_stdin) {
            fail("--input/--stdin are only valid in analyze mode");
        }
    }

    return config;
}

} // namespace pr
