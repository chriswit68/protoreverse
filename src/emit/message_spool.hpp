#pragma once

#include "analysis/message_sink.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace pr {

// Writes parsed messages to a temporary binary spool and later replays them so
// the final inferred schema can be used for decoding.
class MessageSpool final : public MessageSink {
public:
    MessageSpool() = default;
    ~MessageSpool() override;

    MessageSpool(const MessageSpool&) = delete;
    MessageSpool& operator=(const MessageSpool&) = delete;

    bool open(std::string& error);
    void record(const MessageRecord& message) override;
    bool flush(std::string& error);

    // Switches from writing to reading. All buffered records must have been
    // flushed first.
    bool rewind(std::string& error);
    bool next(MessageRecord& message, bool& end, std::string& error);

    const std::string& path() const { return path_; }

private:
    enum class Mode {
        Closed,
        Writing,
        Reading,
    };

    void close();
    void cleanup();

    std::string path_;
    std::fstream stream_;
    Mode mode_ = Mode::Closed;
    std::string write_error_;
    std::vector<std::uint8_t> payload_;
};

} // namespace pr
