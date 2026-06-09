#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

namespace xls20::net {

namespace beast = boost::beast;
namespace asio  = boost::asio;
namespace ssl   = asio::ssl;
using tcp       = asio::ip::tcp;

/// Invoked for every text frame received from the XRPL WebSocket stream.
using MessageCallback = std::function<void(std::string_view)>;

/**
 * @brief Async TLS-secured WebSocket client for the XRPL streaming API.
 *
 * Phase 2 additions:
 *  - Heartbeat Ping/Pong: an ASIO timer fires every `ping_interval_s` seconds
 *    and sends a WebSocket ping frame.  If the server does not reply with a
 *    pong before the next ping cycle, the connection is considered stale and
 *    a reconnect is initiated.
 *  - Exponential Backoff Reconnect Controller: on any connection failure the
 *    delay doubles from `reconnect_delay_ms` up to `max_reconnect_delay_ms`
 *    (default ceiling 64 s), then resets to the initial value on success.
 *
 * Usage:
 *   auto client = std::make_shared<WSClient>(ioc, host, port, path, cb);
 *   client->connect();
 *   ioc.run();
 */
class WSClient : public std::enable_shared_from_this<WSClient> {
public:
    WSClient(asio::io_context& ioc,
             std::string        host,
             std::string        port,
             std::string        path,
             MessageCallback    callback,
             int                reconnect_delay_ms     = 2'000,
             int                max_reconnect_delay_ms = 64'000,
             int                ping_interval_s        = 30);

    /// Begin the async connect chain.  Non-blocking.
    void connect();

    /// Close the WebSocket gracefully and stop all reconnection/heartbeat.
    void disconnect();

    /// Asynchronously send a UTF-8 text frame.
    void send(const std::string& message);

private:
    // ── Async connect chain ───────────────────────────────────────────────────
    void on_resolve(beast::error_code ec, tcp::resolver::results_type results);
    void on_connect(beast::error_code ec,
                    tcp::resolver::results_type::endpoint_type ep);
    void on_ssl_handshake(beast::error_code ec);
    void on_ws_handshake(beast::error_code ec);
    void on_write(beast::error_code ec, std::size_t bytes);
    void on_read(beast::error_code ec, std::size_t bytes);

    void do_read();
    void reconnect();
    void subscribe_to_transactions();
    void make_stream();  ///< (Re)construct the WS stream on each (re)connect.

    // ── Heartbeat (Ping / Pong) ───────────────────────────────────────────────
    // Pong handling is done via ws_->control_callback() registered in
    // on_ws_handshake() — no separate on_pong() method needed.
    void schedule_ping();
    void on_ping_timer(beast::error_code ec);

    // ── Members ───────────────────────────────────────────────────────────────
    asio::io_context& ioc_;
    ssl::context      ssl_ctx_;
    tcp::resolver     resolver_;

    using StreamType =
        beast::websocket::stream<beast::ssl_stream<beast::tcp_stream>>;
    std::unique_ptr<StreamType> ws_;

    beast::flat_buffer buffer_;

    std::string     host_;
    std::string     port_;
    std::string     path_;
    MessageCallback on_message_;

    // ── Reconnect state ───────────────────────────────────────────────────────
    bool               running_{false};
    int                reconnect_delay_ms_;      ///< Current back-off delay
    int const          reconnect_delay_init_ms_; ///< Initial delay (never mutated)
    int const          max_reconnect_delay_ms_;
    asio::steady_timer reconnect_timer_;

    // ── Heartbeat state ───────────────────────────────────────────────────────
    int const          ping_interval_s_;
    asio::steady_timer ping_timer_;
    bool               pong_received_{true};  ///< Set false on ping, true on pong
};

} // namespace xls20::net
