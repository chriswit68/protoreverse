#pragma once

#include "analysis/message_sink.hpp"
#include "cli.hpp"
#include "framing/framer.hpp"
#include "framing/learner.hpp"
#include "infer/schema.hpp"
#include "wire/validator.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pr {

class StreamAnalyzer {
public:
    StreamAnalyzer(std::uint64_t session_id, FramingLearner& learner, SchemaStore& schema,
                   const Config& config, MessageSink* message_sink = nullptr);

    void feed(ByteSpan data);
    void process_available();
    void finish();

    bool locked() const { return learner_.locked(); }
    bool fatal_error() const { return fatal_error_; }
    const std::string& fatal_message() const { return fatal_message_; }
    std::uint64_t frames_processed() const { return frames_processed_; }
    std::uint64_t bytes_received() const { return bytes_received_; }
    std::uint64_t errors() const { return errors_; }
    std::uint64_t session_id() const { return session_id_; }

    bool has_first_message() const { return first_message_captured_; }
    const std::vector<std::uint8_t>& first_message_payload() const {
        return first_message_payload_;
    }
    std::uint64_t first_message_frame_type() const { return first_message_frame_type_; }

private:
    void process_locked();
    void process_framed();
    void process_raw();
    void handle_error(const std::string& message);
    void record_message(ByteSpan payload, std::optional<std::uint64_t> frame_type);
    void maybe_save_first_message(ByteSpan bytes);
    void ensure_framer();
    std::string session_prefix() const;

    std::uint64_t session_id_;
    FramingLearner& learner_;
    SchemaStore& schema_;
    const Config& config_;
    MessageSink* message_sink_ = nullptr;
    std::string first_message_path_;
    bool first_message_saved_ = false;
    bool first_message_captured_ = false;
    std::vector<std::uint8_t> first_message_payload_;
    std::uint64_t first_message_frame_type_ = 0;
    ParseLimits limits_;

    std::vector<std::uint8_t> buffer_;
    std::size_t skip_remaining_ = 0;
    std::unique_ptr<Framer> framer_;
    bool framer_initialized_ = false;
    bool raw_mode_ = false;
    bool first_data_ = true;
    bool fatal_error_ = false;
    std::string fatal_message_;

    std::uint64_t bytes_received_ = 0;
    std::uint64_t frames_processed_ = 0;
    std::uint64_t errors_ = 0;
};

} // namespace pr
