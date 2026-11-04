#include "analysis/stream_analyzer.hpp"
#include "cli.hpp"
#include "emit/proto_writer.hpp"
#include "framing/learner.hpp"
#include "infer/schema.hpp"
#include "infer/type_resolver.hpp"
#include "log.hpp"
#include "net/server.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace pr {
namespace {

int run_analyze(const Config& config) {
    ParseLimits limits{1'000'000, 64, config.max_frame};
    FramingLearner learner(config.framing, config.learning_bytes, config.learning_frames,
                           config.learning_timeout, config.max_frame,
                           config.max_learning_memory, limits);
    SchemaStore schema(false, !config.no_samples, limits);
    StreamAnalyzer analyzer(1, learner, schema, config);

    std::ifstream file;
    std::istream* input = nullptr;
    if (config.input_stdin) {
        input = &std::cin;
    } else {
        file.open(config.input_path, std::ios::binary);
        if (!file) {
            log_error("input", "cannot open input file: ", config.input_path);
            return 3;
        }
        input = &file;
    }

    std::array<std::uint8_t, 64 * 1024> buffer{};
    std::uint64_t total = 0;
    while (*input) {
        input->read(reinterpret_cast<char*>(buffer.data()),
                    static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input->gcount();
        if (count <= 0) {
            break;
        }
        analyzer.feed(ByteSpan(buffer.data(), static_cast<std::size_t>(count)));
        total += static_cast<std::uint64_t>(count);

        if (analyzer.fatal_error()) {
            break;
        }
        if (config.max_messages > 0 &&
            analyzer.frames_processed() >= config.max_messages) {
            break;
        }
    }

    analyzer.finish();

    if (analyzer.fatal_error()) {
        log_error("analyze", "stopped after parse error: ", analyzer.fatal_message());
        return 5;
    }

    if (!learner.locked()) {
        learner.force_lock();
    }

    if (config.learning_only) {
        if (learner.candidates().empty()) {
            log_info("learn", "framing locked to ", to_string(learner.spec()),
                     " (explicit framing or no candidate data)");
        } else {
            std::size_t shown = 0;
            for (const CandidateScore& candidate : learner.candidates()) {
                if (!candidate.valid || shown >= 10) {
                    continue;
                }
                log_info("learn", candidate.label, " score=", candidate.score,
                         " frames=", candidate.frames, " bytes=", candidate.bytes,
                         candidate.reason.empty() ? "" : " reason=" + candidate.reason);
                ++shown;
            }
        }
        return 0;
    }

    const bool raw_mode =
        learner.locked() && learner.spec().kind == FramingKind::Raw;
    ResolveOptions resolve_options;
    resolve_options.root_name = config.root_name;
    resolve_options.proto2 = config.proto2;
    resolve_options.raw_mode = raw_mode;

    const ResolvedModel model = resolve_schema(schema.root(), resolve_options);

    ProtoOptions proto_options;
    proto_options.proto2 = config.proto2;
    proto_options.package = config.package;
    proto_options.framing_label = learner.locked() ? to_string(learner.spec()) : "auto";
    proto_options.framing_confidence = learner.confidence();
    proto_options.sessions = 1;
    proto_options.bytes = analyzer.bytes_received();
    proto_options.frames = analyzer.frames_processed();

    const std::string content = render_proto(model, proto_options);
    std::string error;
    if (!write_proto_file(config.output_path, content, error)) {
        log_error("output", "failed to write ", config.output_path, ": ", error);
        return 4;
    }

    log_info("summary", "input=", config.input_stdin ? "stdin" : config.input_path,
             ", bytes=", total, ", frames=", analyzer.frames_processed(),
             ", errors=", analyzer.errors(),
             ", framing=", learner.locked() ? to_string(learner.spec()) : "auto",
             ", confidence=", learner.confidence(),
             ", mode=", raw_mode ? "raw" : "framed");
    log_info("output", "wrote ", config.output_path, " (", model.messages.size(),
             " messages, ", model.field_count, " fields)");
    return 0;
}

} // namespace
} // namespace pr

int main(int argc, char** argv) {
    try {
        const pr::Config config = pr::parse_args(argc, argv);
        pr::set_log_level(config.level);

        if (config.mode == pr::RunMode::Serve) {
            pr::Server server(config);
            return server.run();
        }
        return pr::run_analyze(config);
    } catch (const pr::CliError& error) {
        std::cerr << "error: " << error.what() << "\n\n";
        pr::print_usage(std::cerr);
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
