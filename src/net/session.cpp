#include "net/session.hpp"

#include "log.hpp"

#include <boost/asio/error.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>

#include <utility>

namespace pr {

Session::Session(boost::asio::ip::tcp::socket socket, std::uint64_t id,
                 FramingLearner& learner, SchemaStore& schema, const Config& config,
                 SessionCallbacks callbacks,
                 std::shared_ptr<const std::vector<std::uint8_t>> greeting)
    : socket_(std::move(socket)),
      idle_timer_(socket_.get_executor()),
      id_(id),
      config_(config),
      callbacks_(std::move(callbacks)),
      analyzer_(id, learner, schema, config),
      greeting_(std::move(greeting)) {}

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
    send_greeting();
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
            self->analyzer_.process_available();

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
    if (!closed_) {
        analyzer_.process_available();
    }
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
