#include "network/http_client.hpp"

#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/err.h>

namespace xls20::net {

namespace beast = boost::beast;
namespace asio  = boost::asio;
namespace ssl   = asio::ssl;
namespace http  = beast::http;
using tcp       = asio::ip::tcp;

// ── URL parser ────────────────────────────────────────────────────────────────

namespace {

struct ParsedUrl {
    bool        tls    = false;
    std::string host;
    std::string port;
    std::string target = "/";
};

[[nodiscard]] std::optional<ParsedUrl> parse_url(std::string_view url) noexcept {
    ParsedUrl r;

    if (url.starts_with("https://")) {
        r.tls  = true;
        r.port = "443";
        url.remove_prefix(8);
    } else if (url.starts_with("http://")) {
        r.tls  = false;
        r.port = "80";
        url.remove_prefix(7);
    } else {
        return std::nullopt;
    }

    // Split authority (host[:port]) from path
    auto slash = url.find('/');
    std::string_view authority = (slash != url.npos) ? url.substr(0, slash) : url;
    r.target = (slash != url.npos) ? std::string(url.substr(slash)) : "/";

    // Split host from optional inline port
    auto colon = authority.find(':');
    if (colon != authority.npos) {
        r.host = std::string(authority.substr(0, colon));
        r.port = std::string(authority.substr(colon + 1));
    } else {
        r.host = std::string(authority);
    }

    if (r.host.empty()) return std::nullopt;
    return r;
}

// ── Plain HTTP session ────────────────────────────────────────────────────────
//
// Self-owns its lifetime via shared_ptr.  Destroyed automatically when the
// async chain completes (last shared_ptr reference drops).

class PlainHttpSession
    : public std::enable_shared_from_this<PlainHttpSession> {
public:
    PlainHttpSession(asio::io_context& ioc,
                     ParsedUrl         url,
                     http::verb        method,
                     std::string       body,
                     HttpCallback      cb)
        : resolver_(ioc)
        , stream_(ioc)
        , url_(std::move(url))
        , cb_(std::move(cb))
    {
        build_request(method, std::move(body));
    }

    void start() {
        stream_.expires_after(std::chrono::seconds(10));
        resolver_.async_resolve(url_.host, url_.port,
            beast::bind_front_handler(&PlainHttpSession::on_resolve,
                                      shared_from_this()));
    }

private:
    void build_request(http::verb method, std::string body) {
        req_.method(method);
        req_.target(url_.target);
        req_.version(11);
        req_.set(http::field::host,       url_.host);
        req_.set(http::field::user_agent, "XLS20PredictorAgent/1.4.0");
        req_.set(http::field::connection, "close");
        if (method == http::verb::post && !body.empty()) {
            req_.set(http::field::content_type, "application/json");
            req_.body() = std::move(body);
            req_.prepare_payload();
        }
    }

    void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
        if (ec) return fail(ec, "resolve");
        stream_.expires_after(std::chrono::seconds(10));
        stream_.async_connect(results,
            beast::bind_front_handler(&PlainHttpSession::on_connect,
                                      shared_from_this()));
    }

    void on_connect(beast::error_code ec,
                    tcp::resolver::results_type::endpoint_type /*ep*/) {
        if (ec) return fail(ec, "connect");
        stream_.expires_after(std::chrono::seconds(15));
        http::async_write(stream_, req_,
            beast::bind_front_handler(&PlainHttpSession::on_write,
                                      shared_from_this()));
    }

    void on_write(beast::error_code ec, std::size_t /*bytes*/) {
        if (ec) return fail(ec, "write");
        http::async_read(stream_, buf_, res_,
            beast::bind_front_handler(&PlainHttpSession::on_read,
                                      shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t /*bytes*/) {
        if (ec) return fail(ec, "read");
        beast::error_code ignored;
        stream_.socket().shutdown(tcp::socket::shutdown_both, ignored);
        if (cb_) cb_(true, res_.result_int(), std::move(res_.body()));
    }

    void fail(beast::error_code ec, const char* where) {
        std::cerr << "[HttpClient/plain] " << where << ": "
                  << ec.message() << '\n';
        if (cb_) cb_(false, 0, {});
    }

    tcp::resolver                    resolver_;
    beast::tcp_stream                stream_;
    beast::flat_buffer               buf_;
    http::request<http::string_body> req_;
    http::response<http::string_body>res_;
    ParsedUrl                        url_;
    HttpCallback                     cb_;
};

// ── TLS HTTP session ──────────────────────────────────────────────────────────

class TlsHttpSession
    : public std::enable_shared_from_this<TlsHttpSession> {
public:
    TlsHttpSession(asio::io_context& ioc,
                   ParsedUrl         url,
                   http::verb        method,
                   std::string       body,
                   HttpCallback      cb)
        : resolver_(ioc)
        , ssl_ctx_(ssl::context::tlsv12_client)
        , stream_(ioc, ssl_ctx_)
        , url_(std::move(url))
        , cb_(std::move(cb))
    {
        ssl_ctx_.set_default_verify_paths();
        ssl_ctx_.set_verify_mode(ssl::verify_peer);
        build_request(method, std::move(body));
    }

    void start() {
        beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(10));
        resolver_.async_resolve(url_.host, url_.port,
            beast::bind_front_handler(&TlsHttpSession::on_resolve,
                                      shared_from_this()));
    }

private:
    void build_request(http::verb method, std::string body) {
        req_.method(method);
        req_.target(url_.target);
        req_.version(11);
        req_.set(http::field::host,       url_.host);
        req_.set(http::field::user_agent, "XLS20PredictorAgent/1.4.0");
        req_.set(http::field::connection, "close");
        if (method == http::verb::post && !body.empty()) {
            req_.set(http::field::content_type, "application/json");
            req_.body() = std::move(body);
            req_.prepare_payload();
        }
    }

    void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
        if (ec) return fail(ec, "resolve");
        beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(10));
        beast::get_lowest_layer(stream_).async_connect(results,
            beast::bind_front_handler(&TlsHttpSession::on_connect,
                                      shared_from_this()));
    }

    void on_connect(beast::error_code ec,
                    tcp::resolver::results_type::endpoint_type /*ep*/) {
        if (ec) return fail(ec, "connect");

        // TLS SNI — required by most servers to serve the correct certificate.
        if (!SSL_set_tlsext_host_name(stream_.native_handle(),
                                      url_.host.c_str())) {
            beast::error_code ssl_ec{
                static_cast<int>(::ERR_get_error()),
                asio::error::get_ssl_category()};
            return fail(ssl_ec, "SNI");
        }

        beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(10));
        stream_.async_handshake(ssl::stream_base::client,
            beast::bind_front_handler(&TlsHttpSession::on_handshake,
                                      shared_from_this()));
    }

    void on_handshake(beast::error_code ec) {
        if (ec) return fail(ec, "TLS handshake");
        beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(15));
        http::async_write(stream_, req_,
            beast::bind_front_handler(&TlsHttpSession::on_write,
                                      shared_from_this()));
    }

    void on_write(beast::error_code ec, std::size_t /*bytes*/) {
        if (ec) return fail(ec, "write");
        http::async_read(stream_, buf_, res_,
            beast::bind_front_handler(&TlsHttpSession::on_read,
                                      shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t /*bytes*/) {
        if (ec) return fail(ec, "read");
        // Best-effort graceful TLS shutdown — errors are silently ignored since
        // the response has already been fully received.
        beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(5));
        stream_.async_shutdown(
            [self = shared_from_this()](beast::error_code /*ignored*/) {
                if (self->cb_)
                    self->cb_(true,
                              self->res_.result_int(),
                              std::move(self->res_.body()));
            });
    }

    void fail(beast::error_code ec, const char* where) {
        std::cerr << "[HttpClient/tls] " << where << ": "
                  << ec.message() << '\n';
        if (cb_) cb_(false, 0, {});
    }

    tcp::resolver                            resolver_;
    ssl::context                             ssl_ctx_;
    beast::ssl_stream<beast::tcp_stream>     stream_;
    beast::flat_buffer                       buf_;
    http::request<http::string_body>         req_;
    http::response<http::string_body>        res_;
    ParsedUrl                                url_;
    HttpCallback                             cb_;
};

// ── Internal dispatcher ────────────────────────────────────────────────────────

static void dispatch_request(asio::io_context& ioc,
                              const std::string& url,
                              http::verb         method,
                              const std::string& body,
                              HttpCallback       cb) {
    auto parsed = parse_url(url);
    if (!parsed) {
        std::cerr << "[HttpClient] Unsupported or malformed URL: " << url << '\n';
        if (cb) cb(false, 0, {});
        return;
    }

    if (parsed->tls) {
        std::make_shared<TlsHttpSession>(
            ioc, std::move(*parsed), method, body, std::move(cb))->start();
    } else {
        std::make_shared<PlainHttpSession>(
            ioc, std::move(*parsed), method, body, std::move(cb))->start();
    }
}

} // anonymous namespace

// ── Public API ─────────────────────────────────────────────────────────────────

void async_http_get(asio::io_context& ioc,
                    const std::string& url,
                    HttpCallback cb) {
    dispatch_request(ioc, url, http::verb::get, {}, std::move(cb));
}

void async_http_post(asio::io_context& ioc,
                     const std::string& url,
                     const std::string& json_body,
                     HttpCallback cb) {
    dispatch_request(ioc, url, http::verb::post, json_body, std::move(cb));
}

} // namespace xls20::net
