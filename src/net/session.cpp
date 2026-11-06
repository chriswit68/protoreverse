#include "net/session.hpp"

#include "framing/encoder.hpp"
#include "log.hpp"

#include <boost/asio/error.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>

#include <utility>

namespace pr {

Session::Session(boost::asio::ip::tcp::socket socket, std::uint64_t id,
                 FramingLearner& learner, SchemaStore& schema, const Config& config,
                 SessionCallbacks callbacks,
                 std::shared_ptr<const std::vector<std::uint8_t>> greeting_wire,
                 std::shared_ptr<const std::vector<std::uint8_t>> greeting_payload,
                 MessageSink* message_sink)
    : socket_(std::move(socket)),
      idle_timer_(socket_.get_executor()),
      alive_timer_(socket_.get_executor()),
      id_(id),
      config_(config),
      learner_(learner),
      callbacks_(std::move(callbacks)),
      analyzer_(id, learner, schema, config, message_sink),
      greeting_(std::move(greeting_wire)),
      greeting_payload_(std::move(greeting_payload)) {}

void Session::start() {
    boost::system::error_code ec;
    const auto endpoint = socket_.remote_endpoint(ec);
    if (!ec) {
        log_info("net", "session ", id_, " connected from ", endpoint.address().to_string(),
                 ":", endpoint.port());
    } else {
        log_info("net", "session ", id_, " connected");
    }

    arm_idle_timer();
    if (!config_.reply_with_first_message) {
        if (greeting_payload_) {
            // Auto framing could not be encoded at startup. If a previous session
            // already locked the learner, the greeting can be sent right away.
            if (learner_.locked()) {
                maybe_send_delayed_greeting(false);
            }
        } else {
            send_greeting();
        }
    }
    do_read();
}

void Session::send_greeting() {
    if (!greeting_ || greeting_->empty() || greeting_sent_ || closed_) {
        return;
    }
    greeting_sent_ = true;

    auto self = shared_from_this();
    boost::asio::async_write(
        socket_, boost::asio::buffer(*greeting_),
        [self](const boost::system::error_code& error, std::size_t bytes) {
            if (error) {
                if (error != boost::asio::error::operation_aborted && !self->closed_) {
                    log_warn("net", "session ", self->id_,
                             " greeting send failed: ", error.message());
                    self->handle_closed();
                }
                return;
            }
            if (!self->closed_) {
                log_info("net", "session ", self->id_, " sent greeting (", bytes,
                         " bytes)");
            }
        });
}

void Session::arm_alive_timer() {
    if (closed_ || !config_.alive_message || alive_started_) {
        return;
    }
    alive_started_ = true;
    alive_timer_.expires_after(config_.alive_interval);
    auto self = shared_from_this();
    alive_timer_.async_wait([self](const boost::system::error_code& error) {
        if (error == boost::asio::error::operation_aborted || error || self->closed_) {
            return;
        }
        self->send_alive();
    });
}

void Session::send_alive() {
    if (closed_ || !config_.alive_message) {
        return;
    }

    // The keep-alive is a header-only frame: type field + zero length, no
    // protobuf payload. It reuses the learned framing; an explicit
    // --alive-message-type overrides the message type in the header.
    std::vector<std::uint8_t> empty_payload;
    std::vector<std::uint8_t> wire;
    std::string error;
    FramingSpec framing_spec = learner_.spec();
    if (config_.alive_message_type_set) {
        framing_spec.type_value = config_.alive_message_type;
    }
    const bool encoded = encode_frame(
        framing_spec, ByteSpan(empty_payload.data(), empty_payload.size()), wire, error);
    if (!encoded) {
        log_error("net", "session ", id_, " alive frame encoding failed: ", error);
        return;
    }

    auto self = shared_from_this();
    boost::asio::async_write(
        socket_, boost::asio::buffer(wire),
        [self](const boost::system::error_code& error, std::size_t bytes) {
            if (error) {
                if (error != boost::asio::error::operation_aborted && !self->closed_) {
                    log_warn("net", "session ", self->id_, " alive send failed: ", error.message());
                    self->handle_closed();
                }
                return;
            }
            if (!self->closed_) {
                log_info("net", "session ", self->id_, " sent alive keep-alive (", bytes,
                         " bytes)");
            }
            self->arm_alive_timer();
        });
}

void Session::maybe_send_delayed_greeting(bool require_frame) {
    if (closed_ || greeting_sent_ || greeting_skipped_) {
        return;
    }
    if (!config_.greeting.framing ||
        config_.greeting.framing->kind != FramingKind::Auto) {
        return;
    }
    if (!learner_.locked()) {
        return;
    }
    if (require_frame && analyzer_.frames_processed() == 0) {
        return;
    }

    FramingSpec framing;
    std::string error;
    if (!resolve_greeting_framing(config_.greeting, learner_.spec(), framing, error)) {
        greeting_skipped_ = true;
        log_warn("net", "session ", id_, " greeting skipped: ", error);
        return;
    }

    if (config_.greeting.message_type_set && framing.kind != FramingKind::TypedLength) {
        log_warn("net", "session ", id_,
                 " --greeting-type ignored: learned framing has no type field");
    }

    std::vector<std::uint8_t> payload_bytes;
    std::uint64_t type_value = config_.greeting.message_type;
    if (config_.reply_with_first_message) {
        // Reply with the first observed client message.
        if (!analyzer_.has_first_message()) {
            return;
        }
        payload_bytes = analyzer_.first_message_payload();
        type_value = analyzer_.first_message_frame_type();
    } else {
        if (!greeting_payload_) {
            return;
        }
        payload_bytes = *greeting_payload_;
    }

    std::vector<std::uint8_t> wire;
    const bool encoded = config_.reply_with_first_message
                             ? encode_greeting_wire(type_value, framing,
                                                   ByteSpan(payload_bytes.data(),
                                                            payload_bytes.size()),
                                                   wire, error)
                             : encode_greeting_wire(config_.greeting, framing,
                                                   ByteSpan(payload_bytes.data(),
                                                            payload_bytes.size()),
                                                   wire, error);
    if (!encoded) {
        log_error("net", "session ", id_, " greeting encoding failed: ", error);
        close();
        return;
    }

    log_info("net", "session ", id_, " sending deferred greeting with framing ",
             to_string(framing));
    greeting_ = std::make_shared<const std::vector<std::uint8_t>>(std::move(wire));
    send_greeting();

    if (config_.alive_message) {
        // The keep-alive reuses the framing the greeting was sent with.
        arm_alive_timer();
    }
}

void Session::do_read() {
    if (closed_) {
        return;
    }

    auto self = shared_from_this();
    socket_.async_read_some(
        boost::asio::buffer(read_buffer_),
        [self](const boost::system::error_code& error, std::size_t bytes) {
            if (error) {
                if (error != boost::asio::error::operation_aborted) {
                    log_debug("net", "session ", self->id_, " read ended: ", error.message());
                }
                self->handle_closed();
                return;
            }

            if (self->closed_) {
                return;
            }

            self->arm_idle_timer();
            self->analyzer_.feed(ByteSpan(self->read_buffer_.data(), bytes));
            self->process_available();

            if (self->callbacks_.on_data) {
                self->callbacks_.on_data(*self);
            }

            if (!self->closed_) {
                self->do_read();
            }
        });
}

void Session::arm_idle_timer() {
    if (config_.idle_timeout.count() <= 0) {
        return;
    }

    idle_timer_.expires_after(config_.idle_timeout);
    auto self = shared_from_this();
    idle_timer_.async_wait([self](const boost::system::error_code& error) {
        if (error == boost::asio::error::operation_aborted || self->closed_) {
            return;
        }
        if (error) {
            return;
        }
        log_info("net", "session ", self->id_, " idle timeout");
        self->close();
    });
}

void Session::process_available() {
    if (closed_) {
        return;
    }

    analyzer_.process_available();
    maybe_send_delayed_greeting(true);
}

void Session::finish_analysis() {
    if (finished_) {
        return;
    }
    finished_ = true;
    analyzer_.finish();
}

void Session::close() {
    if (closed_) {
        return;
    }
    closed_ = true;

    finish_analysis();

    boost::system::error_code ec;
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);

    idle_timer_.cancel();
    alive_timer_.cancel();

    if (!notified_closed_) {
        notified_closed_ = true;
        if (callbacks_.on_closed) {
            callbacks_.on_closed(*this);
        }
    }
}

void Session::handle_closed() {
    if (closed_) {
        return;
    }
    closed_ = true;

    boost::system::error_code ec;
    socket_.close(ec);
    idle_timer_.cancel();
    alive_timer_.cancel();

    // A client may disconnect before the global framing lock was acquired.
    // Flush the analyzer so buffered bytes are not silently lost.
    finish_analysis();

    log_info("net", "session ", id_, " closed after ", analyzer_.bytes_received(), " bytes, ",
             analyzer_.frames_processed(), " frames, ", analyzer_.errors(), " errors");

    if (!notified_closed_) {
        notified_closed_ = true;
        if (callbacks_.on_closed) {
            callbacks_.on_closed(*this);
        }
    }
}

} // namespace pr
