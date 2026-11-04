#include "net/server.hpp"

#include "greeting/greeting.hpp"
#include "log.hpp"

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <csignal>
#include <stdexcept>
#include <utility>

namespace pr {

Server::Server(Config config)
    : config_(std::move(config)),
      io_(),
      acceptor_(io_),
      signals_(io_, SIGINT, SIGTERM),
      poll_timer_(io_),
      limits_{1'000'000, 64, config_.max_frame},
      learner_(config_.framing, config_.learning_bytes, config_.learning_frames,
               config_.learning_timeout, config_.max_frame, config_.max_learning_memory,
               limits_, config_.early_lock),
      schema_(false, !config_.no_samples, limits_) {
    if (config_.greeting.enabled()) {
        std::vector<std::uint8_t> wire;
        std::string error;
        if (!build_greeting_wire(config_.greeting, config_.max_frame, wire, error)) {
            throw std::runtime_error("invalid greeting: " + error);
        }
        greeting_wire_ = std::make_shared<const std::vector<std::uint8_t>>(std::move(wire));
    }

    boost::system::error_code ec;
    const auto address = boost::asio::ip::make_address(config_.bind_address, ec);
    if (ec) {
        throw std::runtime_error("invalid bind address: " + ec.message());
    }

    const tcp::endpoint endpoint(address, config_.port);
    acceptor_.open(endpoint.protocol(), ec);
    if (ec) {
        throw std::runtime_error("cannot open acceptor: " + ec.message());
    }
    acceptor_.set_option(tcp::acceptor::reuse_address(true), ec);
    if (ec) {
        throw std::runtime_error("cannot set reuse_address: " + ec.message());
    }
    acceptor_.bind(endpoint, ec);
    if (ec) {
        throw std::runtime_error("cannot bind to " + config_.bind_address + ":" +
                                 std::to_string(config_.port) + ": " + ec.message());
    }
    acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) {
        throw std::runtime_error("cannot listen: " + ec.message());
    }
}

int Server::run() {
    started_at_ = std::chrono::steady_clock::now();

    boost::system::error_code ec;
    const auto endpoint = acceptor_.local_endpoint(ec);
    if (!ec) {
        log_info("net", "listening on ", endpoint.address().to_string(), ":",
                 endpoint.port());
    }
    log_info("net", "output file: ", config_.output_path);
    if (greeting_wire_) {
        log_info("net", "greeting: ", greeting_wire_->size(), " wire bytes, framing ",
                 to_string(*config_.greeting.framing));
    }

    do_accept();
    arm_poll_timer();

    signals_.async_wait([this](const boost::system::error_code& error, int signal_number) {
        on_signal(error, signal_number);
    });

    io_.run();
    return exit_code_;
}

void Server::do_accept() {
    if (stopping_) {
        return;
    }

    acceptor_.async_accept([this](const boost::system::error_code& error,
                                  tcp::socket socket) {
        if (error) {
            if (error != boost::asio::error::operation_aborted && !stopping_) {
                log_error("net", "accept failed: ", error.message());
                // Accept errors can be transient (for example EMFILE). Keep the
                // acceptor alive instead of silently leaving the server deaf.
                do_accept();
            }
            return;
        }

        const std::uint64_t id = next_session_id_++;
        SessionCallbacks callbacks;
        callbacks.on_data = [this](Session& session) { on_session_data(session); };
        callbacks.on_closed = [this](Session& session) { on_session_closed(session); };

        auto session = std::make_shared<Session>(std::move(socket), id, learner_, schema_,
                                                 config_, std::move(callbacks),
                                                 greeting_wire_);
        sessions_[id] = session;
        ++sessions_started_;
        session->start();

        do_accept();
    });
}

void Server::arm_poll_timer() {
    if (stopping_) {
        return;
    }
    poll_timer_.expires_after(std::chrono::milliseconds(200));
    poll_timer_.async_wait([this](const boost::system::error_code& error) {
        if (error || stopping_) {
            return;
        }
        poll();
    });
}

void Server::poll() {
    if (stopping_) {
        return;
    }

    if (!learner_.locked()) {
        learner_.try_lock(false);
    }

    for (const auto& session : session_list()) {
        session->process_available();
        if (session->analyzer().fatal_error()) {
            stop("strict parse error", 5);
            return;
        }
    }

    if (config_.learning_only && learner_.locked()) {
        stop("learning complete");
        return;
    }

    if (config_.max_messages > 0 && total_frames() >= config_.max_messages) {
        stop("maximum number of messages reached");
        return;
    }

    if (config_.run_for.count() > 0 &&
        std::chrono::steady_clock::now() - started_at_ >= config_.run_for) {
        stop("run duration reached");
        return;
    }

    if (config_.once && sessions_started_ > 0 && sessions_.empty()) {
        stop("once requested");
        return;
    }

    maybe_autosave();
    arm_poll_timer();
}

void Server::on_session_data(Session& session) {
    if (stopping_) {
        return;
    }

    if (session.analyzer().fatal_error()) {
        stop("strict parse error", 5);
        return;
    }

    if (config_.learning_only && learner_.locked()) {
        stop("learning complete");
        return;
    }

    if (config_.max_messages > 0 && total_frames() >= config_.max_messages) {
        stop("maximum number of messages reached");
        return;
    }

    maybe_autosave();
}

void Server::on_session_closed(Session& session) {
    const std::uint64_t id = session.id();
    const auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        return;
    }

    closed_bytes_ += session.analyzer().bytes_received();
    closed_frames_ += session.analyzer().frames_processed();
    closed_errors_ += session.analyzer().errors();
    ++closed_sessions_;
    sessions_.erase(it);

    if (config_.once && sessions_started_ > 0 && sessions_.empty() && !stopping_) {
        stop("once requested");
    }
}

void Server::on_signal(const boost::system::error_code& error, int signal_number) {
    if (error) {
        return;
    }
    log_info("net", "signal ", signal_number, " received");
    stop("signal");
}

void Server::stop(const std::string& reason, int exit_code) {
    if (stopping_) {
        return;
    }
    stopping_ = true;
    stop_reason_ = reason;
    exit_code_ = std::max(exit_code_, exit_code);
    log_info("server", "stopping: ", reason);

    boost::system::error_code ec;
    acceptor_.close(ec);
    poll_timer_.cancel();
    signals_.cancel();

    const auto sessions = session_list();
    for (const auto& session : sessions) {
        session->finish_analysis();
    }
    for (const auto& session : sessions) {
        session->close();
    }

    finalize_output(reason);
    io_.stop();
}

void Server::maybe_autosave() {
    if (config_.autosave == 0 || config_.learning_only || !learner_.locked() ||
        stopping_) {
        return;
    }

    const std::uint64_t frames = total_frames();
    if (frames >= last_autosave_frames_ + config_.autosave) {
        write_snapshot(false);
        last_autosave_frames_ = frames;
    }
}

void Server::write_snapshot(bool final_write) {
    if (config_.learning_only) {
        return;
    }

    const bool raw_mode =
        learner_.locked() && learner_.spec().kind == FramingKind::Raw;
    ResolveOptions resolve_options;
    resolve_options.root_name = config_.root_name;
    resolve_options.proto2 = config_.proto2;
    resolve_options.raw_mode = raw_mode;

    const ResolvedModel model = resolve_schema(schema_.root(), resolve_options);
    const std::string content =
        render_proto(model, proto_options());

    std::string error;
    if (!write_proto_file(config_.output_path, content, error)) {
        log_error("output", "failed to write ", config_.output_path, ": ", error);
        exit_code_ = 4;
        return;
    }

    if (final_write) {
        log_info("output", "wrote ", config_.output_path, " (", model.messages.size(),
                 " messages, ", model.field_count, " fields)");
    } else {
        log_info("output", "autosaved ", config_.output_path, " (", model.messages.size(),
                 " messages, ", model.field_count, " fields)");
    }
}

void Server::finalize_output(const std::string& reason) {
    if (finalized_) {
        return;
    }
    finalized_ = true;

    if (!learner_.locked()) {
        learner_.force_lock();
    }

    if (config_.learning_only) {
        if (learner_.candidates().empty()) {
            log_info("learn", "framing locked to ", to_string(learner_.spec()),
                     " (explicit framing or no candidate data)");
        } else {
            std::size_t shown = 0;
            for (const CandidateScore& candidate : learner_.candidates()) {
                if (!candidate.valid || shown >= 10) {
                    continue;
                }
                log_info("learn", candidate.label, " score=", candidate.score,
                         " frames=", candidate.frames, " bytes=", candidate.bytes,
                         candidate.reason.empty() ? "" : " reason=" + candidate.reason);
                ++shown;
            }
        }
    } else {
        write_snapshot(true);
    }

    const bool raw_mode =
        learner_.locked() && learner_.spec().kind == FramingKind::Raw;
    log_info("summary", "reason=", reason, ", sessions=", sessions_started_,
             ", bytes=", total_bytes(), ", frames=", total_frames(),
             ", errors=", total_errors(),
             ", framing=", learner_.locked() ? to_string(learner_.spec()) : "auto",
             ", confidence=", learner_.confidence(), ", mode=", raw_mode ? "raw" : "framed");
}

std::uint64_t Server::total_frames() const {
    std::uint64_t total = closed_frames_;
    for (const auto& [id, session] : sessions_) {
        (void)id;
        total += session->analyzer().frames_processed();
    }
    return total;
}

std::uint64_t Server::total_bytes() const {
    std::uint64_t total = closed_bytes_;
    for (const auto& [id, session] : sessions_) {
        (void)id;
        total += session->analyzer().bytes_received();
    }
    return total;
}

std::uint64_t Server::total_errors() const {
    std::uint64_t total = closed_errors_;
    for (const auto& [id, session] : sessions_) {
        (void)id;
        total += session->analyzer().errors();
    }
    return total;
}

std::vector<std::shared_ptr<Session>> Server::session_list() const {
    std::vector<std::shared_ptr<Session>> result;
    result.reserve(sessions_.size());
    for (const auto& [id, session] : sessions_) {
        (void)id;
        result.push_back(session);
    }
    return result;
}

ProtoOptions Server::proto_options() const {
    ProtoOptions options;
    options.proto2 = config_.proto2;
    options.package = config_.package;
    options.framing_label = learner_.locked() ? to_string(learner_.spec()) : "auto";
    options.framing_confidence = learner_.confidence();
    options.sessions = sessions_started_;
    options.bytes = total_bytes();
    options.frames = total_frames();
    return options;
}

} // namespace pr
