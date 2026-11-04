#pragma once

#include "cli.hpp"
#include "emit/proto_writer.hpp"
#include "framing/learner.hpp"
#include "infer/schema.hpp"
#include "net/session.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pr {

class Server {
public:
    explicit Server(Config config);
    int run();

private:
    using tcp = boost::asio::ip::tcp;

    void do_accept();
    void arm_poll_timer();
    void poll();
    void on_session_data(Session& session);
    void on_session_closed(Session& session);
    void on_signal(const boost::system::error_code& error, int signal_number);

    void stop(const std::string& reason, int exit_code = 0);
    void finalize_output(const std::string& reason);
    void maybe_autosave();
    void write_snapshot(bool final_write);

    std::uint64_t total_frames() const;
    std::uint64_t total_bytes() const;
    std::uint64_t total_errors() const;
    std::vector<std::shared_ptr<Session>> session_list() const;

    ProtoOptions proto_options() const;

    Config config_;
    boost::asio::io_context io_;
    tcp::acceptor acceptor_;
    boost::asio::signal_set signals_;
    boost::asio::steady_timer poll_timer_;

    ParseLimits limits_;
    FramingLearner learner_;
    SchemaStore schema_;

    std::map<std::uint64_t, std::shared_ptr<Session>> sessions_;
    std::uint64_t next_session_id_ = 1;
    std::uint64_t sessions_started_ = 0;
    std::uint64_t closed_sessions_ = 0;
    std::uint64_t closed_bytes_ = 0;
    std::uint64_t closed_frames_ = 0;
    std::uint64_t closed_errors_ = 0;
    std::uint64_t last_autosave_frames_ = 0;
    std::shared_ptr<const std::vector<std::uint8_t>> greeting_wire_;

    bool stopping_ = false;
    bool finalized_ = false;
    std::string stop_reason_;
    int exit_code_ = 0;
    std::chrono::steady_clock::time_point started_at_;
};

} // namespace pr
