#include "network/ws_client.hpp"

#include <chrono>
#include <iostream>

#include <nlohmann/json.hpp>
#include <openssl/err.h>

namespace xls20::net {

using json = nlohmann::json;

// ── Constructor / stream factory ──────────────────────────────────────────────

WSClient::WSClient(asio::io_context& ioc,
                   std::string        host,
                   std::string        port,
                   std::string        path,
                   MessageCallback    callback,
                   int                reconnect_delay_ms,
                   int                max_reconnect_delay_ms,
                   int                ping_interval_s)
    : ioc_(ioc)
    , ssl_ctx_(ssl::context::tlsv12_client)
    , resolver_(ioc)
    , host_(std::move(host))
    , port_(std::move(port))
    , path_(std::move(path))
    , on_message_(std::move(callback))
    , reconnect_delay_ms_(reconnect_delay_ms)
    , reconnect_delay_init_ms_(reconnect_delay_ms)
    , max_reconnect_delay_ms_(max_reconnect_delay_ms)
    , reconnect_timer_(ioc)
    , ping_interval_s_(ping_interval_s)
    , ping_timer_(ioc)
{
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_peer);
    make_stream();
}

void WSClient::make_stream() {
    ws_ = std::make_unique<StreamType>(ioc_, ssl_ctx_);
}

// ── Public API ────────────────────────────────────────────────────────────────

void WSClient::connect() {
    running_ = true;
    resolver_.async_resolve(host_, port_,
        beast::bind_front_handler(&WSClient::on_resolve, shared_from_this()));
}

void WSClient::disconnect() {
    running_ = false;
    // Cancel the heartbeat timer first so no ping fires after we close.
    beast::error_code timer_ec;
    ping_timer_.cancel(timer_ec);
    reconnect_timer_.cancel(timer_ec);

    beast::error_code ec;
    ws_->close(beast::websocket::close_code::normal, ec);
    if (ec && ec != beast::websocket::error::closed)
        std::cerr << "[WSClient] Close error: " << ec.message() << '\n';
}

void WSClient::send(const std::string& message) {
    ws_->async_write(asio::buffer(message),
        beast::bind_front_handler(&WSClient::on_write, shared_from_this()));
}

// ── Async connect chain ───────────────────────────────────────────────────────

void WSClient::on_resolve(beast::error_code ec,
                          tcp::resolver::results_type results) {
    if (ec) {
        std::cerr << "[WSClient] Resolve error: " << ec.message() << '\n';
        reconnect();
        return;
    }
    beast::get_lowest_layer(*ws_).expires_after(std::chrono::seconds(30));
    beast::get_lowest_layer(*ws_).async_connect(results,
        beast::bind_front_handler(&WSClient::on_connect, shared_from_this()));
}

void WSClient::on_connect(beast::error_code ec,
                          tcp::resolver::results_type::endpoint_type /*ep*/) {
    if (ec) {
        std::cerr << "[WSClient] Connect error: " << ec.message() << '\n';
        reconnect();
        return;
    }
    // SNI hostname — required by most TLS servers to select the right certificate.
    if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(),
                                  host_.c_str())) {
        beast::error_code ssl_ec{
            static_cast<int>(::ERR_get_error()),
            asio::error::get_ssl_category()};
        std::cerr << "[WSClient] SNI error: " << ssl_ec.message() << '\n';
        reconnect();
        return;
    }
    beast::get_lowest_layer(*ws_).expires_after(std::chrono::seconds(30));
    ws_->next_layer().async_handshake(ssl::stream_base::client,
        beast::bind_front_handler(&WSClient::on_ssl_handshake,
                                  shared_from_this()));
}

void WSClient::on_ssl_handshake(beast::error_code ec) {
    if (ec) {
        std::cerr << "[WSClient] SSL handshake error: " << ec.message() << '\n';
        reconnect();
        return;
    }
    // Hand off timeout responsibility to the WS layer (keep-alive via Beast).
    beast::get_lowest_layer(*ws_).expires_never();

    ws_->set_option(
        beast::websocket::stream_base::timeout::suggested(
            beast::role_type::client));
    ws_->set_option(beast::websocket::stream_base::decorator(
        [](beast::websocket::request_type& req) {
            req.set(beast::http::field::user_agent,
                    "XLS20PredictorAgent/1.0.0 Boost.Beast");
        }));
    ws_->async_handshake(host_, path_,
        beast::bind_front_handler(&WSClient::on_ws_handshake,
                                  shared_from_this()));
}

void WSClient::on_ws_handshake(beast::error_code ec) {
    if (ec) {
        std::cerr << "[WSClient] WS handshake error: " << ec.message() << '\n';
        reconnect();
        return;
    }
    // ── Successful connection: reset exponential back-off to initial value ────
    reconnect_delay_ms_ = reconnect_delay_init_ms_;
    std::cout << "[WSClient] Connected to wss://" << host_ << path_ << '\n';

    // Install the pong callback so we know when the server replies to our pings.
    ws_->control_callback(
        [self = shared_from_this()](beast::websocket::frame_type kind,
                                    beast::string_view /*payload*/) {
            if (kind == beast::websocket::frame_type::pong) {
                self->pong_received_ = true;
                // (debug) std::cout << "[WSClient] Pong received.\n";
            }
        });

    subscribe_to_transactions();
    schedule_ping();   // start the heartbeat cycle
    do_read();
}

void WSClient::subscribe_to_transactions() {
    // XRPL subscribe command — request the live transaction stream.
    json sub = {
        {"id",      "xls20_agent_stream"},
        {"command", "subscribe"},
        {"streams", json::array({"transactions"})}
    };
    send(sub.dump());
    std::cout << "[WSClient] Subscribed to XRPL transaction stream.\n";
}

void WSClient::on_write(beast::error_code ec, std::size_t /*bytes*/) {
    if (ec)
        std::cerr << "[WSClient] Write error: " << ec.message() << '\n';
}

void WSClient::do_read() {
    ws_->async_read(buffer_,
        beast::bind_front_handler(&WSClient::on_read, shared_from_this()));
}

void WSClient::on_read(beast::error_code ec, std::size_t /*bytes*/) {
    if (ec) {
        if (ec == beast::websocket::error::closed)
            std::cout << "[WSClient] Server closed the connection.\n";
        else
            std::cerr << "[WSClient] Read error: " << ec.message() << '\n';

        // Cancel the heartbeat before reconnecting.
        beast::error_code ignored;
        ping_timer_.cancel(ignored);

        if (running_) reconnect();
        return;
    }

    std::string msg = beast::buffers_to_string(buffer_.data());
    buffer_.consume(buffer_.size());

    if (on_message_) on_message_(msg);

    do_read();  // immediately queue the next read
}

// ── Heartbeat (Ping / Pong) ───────────────────────────────────────────────────

void WSClient::schedule_ping() {
    if (!running_) return;

    ping_timer_.expires_after(std::chrono::seconds(ping_interval_s_));
    ping_timer_.async_wait(
        beast::bind_front_handler(&WSClient::on_ping_timer, shared_from_this()));
}

void WSClient::on_ping_timer(beast::error_code ec) {
    if (ec || !running_) return;  // timer cancelled or shutting down

    if (!pong_received_) {
        // The previous ping went unanswered — treat as a silent drop.
        std::cerr << "[WSClient] Heartbeat timeout — no pong received.  "
                     "Forcing reconnect.\n";
        beast::error_code close_ec;
        ping_timer_.cancel(close_ec);
        // Force-close so the pending async_read returns with an error,
        // which will then call reconnect() through the normal path.
        beast::get_lowest_layer(*ws_).close();
        return;
    }

    // Mark that we are waiting for the pong before sending the next ping.
    pong_received_ = false;

    ws_->async_ping({},
        [self = shared_from_this()](beast::error_code ping_ec) {
            if (ping_ec) {
                std::cerr << "[WSClient] Ping error: " << ping_ec.message() << '\n';
                return;
            }
            // (debug) std::cout << "[WSClient] Ping sent.\n";
            // Re-arm for the next interval.
            self->schedule_ping();
        });
}

// ── Exponential Backoff Reconnect Controller ──────────────────────────────────

void WSClient::reconnect() {
    if (!running_) return;

    std::cout << "[WSClient] Reconnecting in "
              << reconnect_delay_ms_ << " ms  (max "
              << max_reconnect_delay_ms_ << " ms)…\n";

    reconnect_timer_.expires_after(
        std::chrono::milliseconds(reconnect_delay_ms_));

    reconnect_timer_.async_wait(
        [self = shared_from_this()](beast::error_code ec) {
            if (ec || !self->running_) return;

            // Rebuild the stream object to start completely fresh.
            self->make_stream();
            self->buffer_.clear();
            self->pong_received_ = true;  // reset heartbeat latch

            // Double the back-off, capped at max_reconnect_delay_ms_.
            self->reconnect_delay_ms_ = std::min(
                self->reconnect_delay_ms_ * 2,
                self->max_reconnect_delay_ms_);

            // Restart the full async connect chain.
            self->resolver_.async_resolve(
                self->host_, self->port_,
                beast::bind_front_handler(&WSClient::on_resolve, self));
        });
}

} // namespace xls20::net
