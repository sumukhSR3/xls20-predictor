#pragma once

#include <string>

#include <boost/asio.hpp>

namespace xls20::net {

/**
 * @brief Asynchronous outbound webhook dispatcher for trading signal alerts.
 *
 * Fires HTTP POST requests non-blocking on the supplied Boost.Asio io_context
 * whenever a tracked collection transitions to a BUY or SELL signal.
 *
 * Design invariants:
 *  - `dispatch()` returns immediately — the actual HTTP I/O runs entirely
 *    inside the io_context event loop.
 *  - Failed requests are logged to stderr and silently dropped.  No retry
 *    queue is maintained, avoiding backlog accumulation during connectivity
 *    outages.
 *  - The dispatcher is disabled (no-op) when constructed with an empty `url`.
 *    Call `enabled()` to check before performing any signal comparison.
 *
 * JSON payload schema (RFC 8259 compliant):
 * @code
 * {
 *   "event":         "XLS20_PREDICTIVE_SIGNAL",
 *   "collection":    "<collection name>",
 *   "signal":        "BUY" | "SELL",
 *   "current_price": <double>,
 *   "lower_band":    <double>,
 *   "timestamp":     "<ISO 8601 UTC, e.g. 2026-06-08T14:10:19Z>"
 * }
 * @endcode
 */
class WebhookDispatcher {
public:
    /**
     * @param ioc  The shared io_context — must outlive this dispatcher.
     * @param url  Full HTTP or HTTPS webhook URL, e.g.
     *             `"http://localhost:9000/alert"`.  Empty string → disabled.
     */
    WebhookDispatcher(boost::asio::io_context& ioc, std::string url);

    /**
     * @brief Build and fire the alert payload asynchronously.
     *
     * @param collection_name  Human-readable collection name.
     * @param signal_str       "BUY" or "SELL".
     * @param current_price    Live price in XRP at signal time.
     * @param lower_band       First-step lower 95 % CI boundary (XRP).
     *
     * The ISO 8601 UTC timestamp is generated internally at call time.
     * Returns immediately; all I/O is deferred to the event loop.
     */
    void dispatch(const std::string& collection_name,
                  const std::string& signal_str,
                  double             current_price,
                  double             lower_band);

    /// Returns false when the URL is empty — dispatcher is a no-op.
    [[nodiscard]] bool enabled() const noexcept { return !url_.empty(); }

private:
    boost::asio::io_context& ioc_;
    std::string              url_;
};

} // namespace xls20::net
