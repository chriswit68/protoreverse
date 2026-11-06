#pragma once

#include "analysis/stream_analyzer.hpp"
#include "cli.hpp"
#include "framing/learner.hpp"
#include "infer/schema.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace pr {

class Session;

struct SessionCallbacks {
    std::function<void(Session&)> on_data;
    std::function<void(Session&)> on_closed;
};

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(boost::asio::ip::tcp::socket socket, std::uint64_t id, FramingLearner& learner,
            SchemaStore& schema, const Config& config, SessionCallbacks callbacks,
            std::shared_ptr<const std::vector<std::uint8_t>> greeting_wire,
            std::shared_ptr<const std::vector<std::uint8_t>> greeting_payload,
            MessageSink* message_sink = nullptr);

    void start();
    void close();
    void finish_analysis();
    void process_available();

    StreamAnalyzer& analyzer() { return analyzer_; }
    const StreamAnalyzer& analyzer() const { return analyzer_; }
    std::uint64_t id() const { return id_; }
    bool closed() const { return closed_; }

private:
    void do_read();
    void send_greeting();
    void maybe_send_delayed_greeting(bool require_frame);
    void arm_idle_timer();
    void arm_alive_timer();
    void send_alive();
    void handle_closed();

    boost::asio::ip::tcp::socket socket_;
    boost::asio::steady_timer idle_timer_;
    boost::asio::steady_timer alive_timer_;
    std::uint64_t id_;
    const Config& config_;
    FramingLearner& learner_;
    SessionCallbacks callbacks_;
    StreamAnalyzer analyzer_;
    std::array<std::uint8_t, 16 * 1024> read_buffer_{};
    std::shared_ptr<const std::vector<std::uint8_t>> greeting_;
    std::shared_ptr<const std::vector<std::uint8_t>> greeting_payload_;
    bool closed_ = false;
    bool notified_closed_ = false;
    bool finished_ = false;
    bool greeting_sent_ = false;
    bool greeting_skipped_ = false;
    bool alive_started_ = false;
};

} // namespace pr
