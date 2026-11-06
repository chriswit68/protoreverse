#include "emit/message_spool.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <system_error>

namespace pr {
namespace {

constexpr std::size_t kHeaderSize = 8 + 8 + 1 + 8 + 8;

void append_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xff));
    }
}

bool read_u64(const std::uint8_t* data, std::uint64_t& value) {
    value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(data[i]) << (i * 8);
    }
    return true;
}

std::string make_temp_path() {
    namespace fs = std::filesystem;

    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t stamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::uint64_t sequence = counter.fetch_add(1);

    std::error_code ec;
    fs::path directory = fs::temp_directory_path(ec);
    if (ec) {
        directory = fs::current_path(ec);
    }
    if (ec) {
        directory = fs::path(".");
    }

    return (directory / ("protoreverse-" + std::to_string(stamp) + "-" +
                         std::to_string(sequence) + ".spool"))
        .string();
}

} // namespace

MessageSpool::~MessageSpool() {
    cleanup();
}

bool MessageSpool::open(std::string& error) {
    if (mode_ != Mode::Closed) {
        error = "spool is already open";
        return false;
    }

    path_ = make_temp_path();
    stream_.open(path_, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!stream_) {
        error = "cannot create temporary spool: " + path_;
        path_.clear();
        return false;
    }

    mode_ = Mode::Writing;
    write_error_.clear();
    return true;
}

void MessageSpool::record(const MessageRecord& message) {
    if (mode_ != Mode::Writing || !write_error_.empty()) {
        return;
    }
    if (!stream_) {
        write_error_ = "spool stream is not writable";
        return;
    }

    std::vector<std::uint8_t> header;
    header.reserve(kHeaderSize);
    append_u64(header, message.session);
    append_u64(header, message.index);
    header.push_back(message.frame_type.has_value() ? 1 : 0);
    append_u64(header, message.frame_type.value_or(0));
    append_u64(header, static_cast<std::uint64_t>(message.payload.size()));

    stream_.write(reinterpret_cast<const char*>(header.data()),
                  static_cast<std::streamsize>(header.size()));
    if (!message.payload.empty()) {
        stream_.write(reinterpret_cast<const char*>(message.payload.data()),
                      static_cast<std::streamsize>(message.payload.size()));
    }
    if (!stream_) {
        write_error_ = "cannot write temporary spool: " + path_;
    }
}

bool MessageSpool::flush(std::string& error) {
    if (!write_error_.empty()) {
        error = write_error_;
        return false;
    }
    if (mode_ == Mode::Closed) {
        return true;
    }
    if (mode_ != Mode::Writing) {
        error = "spool is not in write mode";
        return false;
    }

    stream_.flush();
    if (!stream_) {
        error = "cannot flush temporary spool: " + path_;
        return false;
    }
    return true;
}

bool MessageSpool::rewind(std::string& error) {
    if (!flush(error)) {
        return false;
    }
    if (mode_ == Mode::Closed) {
        error = "spool is not open";
        return false;
    }
    if (mode_ == Mode::Reading) {
        return true;
    }

    stream_.close();
    stream_.clear();
    stream_.open(path_, std::ios::binary | std::ios::in);
    if (!stream_) {
        error = "cannot reopen temporary spool: " + path_;
        return false;
    }

    mode_ = Mode::Reading;
    return true;
}

bool MessageSpool::next(MessageRecord& message, bool& end, std::string& error) {
    end = false;
    if (mode_ != Mode::Reading) {
        error = "spool is not in read mode";
        return false;
    }

    std::array<std::uint8_t, kHeaderSize> header{};
    stream_.read(reinterpret_cast<char*>(header.data()),
                 static_cast<std::streamsize>(header.size()));
    const std::streamsize got = stream_.gcount();
    if (got == 0 && stream_.eof()) {
        end = true;
        return true;
    }
    if (got != static_cast<std::streamsize>(header.size())) {
        error = "truncated temporary spool header";
        return false;
    }

    std::uint64_t session = 0;
    std::uint64_t index = 0;
    std::uint64_t type = 0;
    std::uint64_t size = 0;
    read_u64(header.data(), session);
    read_u64(header.data() + 8, index);
    const bool has_type = header[16] != 0;
    read_u64(header.data() + 17, type);
    read_u64(header.data() + 25, size);

    if (size > static_cast<std::uint64_t>(SIZE_MAX)) {
        error = "temporary spool record is too large";
        return false;
    }

    payload_.resize(static_cast<std::size_t>(size));
    if (size > 0) {
        stream_.read(reinterpret_cast<char*>(payload_.data()),
                     static_cast<std::streamsize>(size));
        if (stream_.gcount() != static_cast<std::streamsize>(size)) {
            error = "truncated temporary spool payload";
            return false;
        }
    }

    message.session = session;
    message.index = index;
    message.frame_type = has_type ? std::optional<std::uint64_t>(type) : std::nullopt;
    message.payload = ByteSpan(payload_.data(), payload_.size());
    return true;
}

void MessageSpool::close() {
    if (stream_.is_open()) {
        stream_.close();
    }
    mode_ = Mode::Closed;
}

void MessageSpool::cleanup() {
    close();
    if (!path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        path_.clear();
    }
}

} // namespace pr
