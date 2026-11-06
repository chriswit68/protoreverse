#include "analysis/stream_analyzer.hpp"

#include "log.hpp"
#include "util/hexdump.hpp"

#include <algorithm>
#include <fstream>

namespace pr {

StreamAnalyzer::StreamAnalyzer(std::uint64_t session_id, FramingLearner& learner,
                               SchemaStore& schema, const Config& config,
                               MessageSink* message_sink)
    : session_id_(session_id),
      learner_(learner),
      schema_(schema),
      config_(config),
      message_sink_(message_sink),
      first_message_path_(config.save_first_message_path),
      limits_{1'000'000, 64, config.max_frame},
      skip_remaining_(config.start_offset) {}

std::string StreamAnalyzer::session_prefix() const {
    return "session " + std::to_string(session_id_) + ": ";
}

void StreamAnalyzer::feed(ByteSpan data) {
    bytes_received_ += data.size();
    if (fatal_error_ || data.empty()) {
        return;
    }

    if (skip_remaining_ > 0) {
        const std::size_t skip = std::min(skip_remaining_, data.size());
        skip_remaining_ -= skip;
        data = data.subspan(skip);
        if (data.empty()) {
            return;
        }
    }

    if (first_data_) {
        first_data_ = false;
        if (config_.hexdump) {
            log_info("session", session_prefix(), "first bytes [", data.size(), "]: ",
                     hexdump(data, config_.hexdump_bytes));
        }
    }

    learner_.contribute(session_id_, data);
    buffer_.insert(buffer_.end(), data.begin(), data.end());

    if (learner_.locked()) {
        process_locked();
        return;
    }

    const std::size_t learning_buffer_limit =
        std::max<std::size_t>(config_.learning_bytes * 2, 256 * 1024);
    if (buffer_.size() > learning_buffer_limit) {
        learner_.force_lock();
        if (learner_.locked()) {
            process_locked();
        } else {
            handle_error("learning buffer exceeded and no framing could be selected");
            buffer_.clear();
        }
    }
}

void StreamAnalyzer::process_available() {
    if (!fatal_error_ && learner_.locked()) {
        process_locked();
    }
}

void StreamAnalyzer::finish() {
    if (fatal_error_) {
        return;
    }

    if (!learner_.locked()) {
        learner_.force_lock();
    }
    if (learner_.locked()) {
        process_locked();
    }

    if (!buffer_.empty()) {
        log_debug("session", session_prefix(), "unparsed trailing bytes at end: ",
                  buffer_.size());
        if (config_.hexdump) {
            log_debug("session", session_prefix(), "tail: ",
                      hexdump(buffer_, config_.hexdump_bytes));
        }
    }
}

void StreamAnalyzer::process_locked() {
    if (fatal_error_ || !learner_.locked()) {
        return;
    }

    ensure_framer();
    if (fatal_error_) {
        return;
    }

    if (raw_mode_) {
        process_raw();
    } else {
        process_framed();
    }
}

void StreamAnalyzer::ensure_framer() {
    if (framer_initialized_) {
        return;
    }
    framer_initialized_ = true;

    const FramingSpec spec = learner_.spec();
    raw_mode_ = spec.kind == FramingKind::Raw;
    if (!raw_mode_) {
        framer_ = make_framer(spec, config_.max_frame);
        if (!framer_) {
            handle_error("no framer available for selected framing");
            return;
        }
    }

    log_debug("session", session_prefix(), "using framing ", to_string(spec));
}

void StreamAnalyzer::process_framed() {
    std::size_t offset = 0;

    while (offset < buffer_.size()) {
        const ByteSpan remaining(buffer_.data() + offset, buffer_.size() - offset);
        std::size_t consumed = 0;
        ByteSpan frame;
        std::string error;
        const FrameStatus status = framer_->next(remaining, consumed, frame, error);

        if (status == FrameStatus::NeedMore) {
            break;
        }
        if (status == FrameStatus::Error) {
            handle_error("framing error: " + error);
            buffer_.clear();
            return;
        }
        if (consumed == 0) {
            handle_error("framer made no progress");
            buffer_.clear();
            return;
        }

        ParsedMessage message;
        std::string parse_error;
        if (!parse_message(frame, message, limits_, 0, parse_error)) {
            handle_error("frame parse failed: " + parse_error + " [bytes " +
                         hexdump(frame, 32) + "]");
            buffer_.clear();
            return;
        }

        schema_.add_message(message);
        ++frames_processed_;
        record_message(frame, framer_->last_frame_type());
        maybe_save_first_message(remaining.first(consumed));
        if (!first_message_captured_) {
            first_message_captured_ = true;
            first_message_frame_type_ = framer_->last_frame_type().value_or(0);
            first_message_payload_ = std::vector<std::uint8_t>(frame.begin(), frame.end());
        }
        offset += consumed;

        if (frames_processed_ % 5000 == 0) {
            log_debug("session", session_prefix(), "processed ", frames_processed_,
                      " frames, buffer=", buffer_.size());
        }
    }

    if (offset > 0) {
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    if (buffer_.size() > config_.max_frame + 1024) {
        handle_error("buffer exceeds maximum frame size");
        buffer_.clear();
    }
}

void StreamAnalyzer::process_raw() {
    std::size_t offset = 0;

    while (offset < buffer_.size()) {
        const ByteSpan remaining(buffer_.data() + offset, buffer_.size() - offset);
        ParsedField field;
        std::size_t consumed = 0;
        bool complete = false;
        std::string error;

        if (!parse_one_field(remaining, field, consumed, complete, limits_, error)) {
            handle_error("raw parse error: " + error + " [bytes " +
                         hexdump(remaining, 32) + "]");
            buffer_.clear();
            return;
        }
        if (!complete) {
            break;
        }
        if (consumed == 0) {
            handle_error("raw parser made no progress");
            buffer_.clear();
            return;
        }

        schema_.add_raw_field(field);
        ++frames_processed_;
        record_message(remaining.first(consumed), std::nullopt);
        maybe_save_first_message(remaining.first(consumed));
        offset += consumed;

        if (frames_processed_ % 5000 == 0) {
            log_debug("session", session_prefix(), "processed ", frames_processed_,
                      " raw fields, buffer=", buffer_.size());
        }
    }

    if (offset > 0) {
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    if (buffer_.size() > config_.max_frame + 1024) {
        handle_error("buffer exceeds maximum frame size");
        buffer_.clear();
    }
}

void StreamAnalyzer::record_message(ByteSpan payload,
                                    std::optional<std::uint64_t> frame_type) {
    if (message_sink_ == nullptr) {
        return;
    }

    MessageRecord record;
    record.session = session_id_;
    record.index = frames_processed_;
    record.frame_type = frame_type;
    record.payload = payload;
    message_sink_->record(record);
}

void StreamAnalyzer::maybe_save_first_message(ByteSpan bytes) {
    if (first_message_saved_ || first_message_path_.empty()) {
        return;
    }
    first_message_saved_ = true;
    if (bytes.empty()) {
        return;
    }

    std::ofstream out(first_message_path_, std::ios::binary | std::ios::trunc);
    if (!out) {
        log_warn("session", session_prefix(), "cannot open first message output file: " +
                                    first_message_path_);
        return;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if (!out) {
        log_warn("session", session_prefix(), "cannot write first message output file: " +
                                    first_message_path_);
        return;
    }

    log_info("session", session_prefix(), "saved first message to ", first_message_path_,
             " (", bytes.size(), " bytes)");
}

void StreamAnalyzer::handle_error(const std::string& message) {
    ++errors_;
    log_warn("session", session_prefix(), message);
    if (config_.hexdump && !buffer_.empty()) {
        log_warn("session", session_prefix(), "buffer: ",
                 hexdump(buffer_, config_.hexdump_bytes));
    }
    if (config_.strict) {
        fatal_error_ = true;
        fatal_message_ = message;
    }
}

} // namespace pr
