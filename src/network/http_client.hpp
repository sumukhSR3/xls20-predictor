#pragma once

#include <functional>
#include <string>

#include <boost/asio.hpp>

namespace xls20::net {

/**
 * @brief Completion callback for all async HTTP operations.
 *
 * Parameters:
 *   bool        success    — false if the transport layer failed (DNS, TCP,
 *                            TLS, read/write error); true on any HTTP response.
 *   unsigned    status     — HTTP status code (e.g. 200, 404, 500).
 *                            Undefined when success == false.
 *   std::string body       — Raw response body.  Empty on failure.
 *
 * The callback is always invoked on the Boost.Asio thread (inside ioc.run()),
 * never on the caller's thread.
 */
using HttpCallback = std::function<void(bool success,
                                        unsigned http_status,
                                        std::string body)>;

/**
 * @brief Fire-and-forget async HTTP/HTTPS GET.
 *
 * Parses `url` (scheme must be http:// or https://), allocates a self-owned
 * session on `ioc`, issues the request, and invokes `cb` with the result.
 *
 * Returns immediately — never blocks the caller.  The session is destroyed
 * automatically when the async chain completes, with no raw ownership or
 * manual lifetime management required by the caller.
 *
 * TLS endpoints use TLSv1.2 with the system CA bundle (verify_peer).
 * Timeouts: 10 s connect, 15 s read.
 */
void async_http_get(boost::asio::io_context& ioc,
                    const std::string&        url,
                    HttpCallback              cb);

/**
 * @brief Fire-and-forget async HTTP/HTTPS POST with a JSON body.
 *
 * `json_body` is sent verbatim as `Content-Type: application/json`.
 * Returns immediately — never blocks.
 */
void async_http_post(boost::asio::io_context& ioc,
                     const std::string&        url,
                     const std::string&        json_body,
                     HttpCallback              cb);

} // namespace xls20::net
