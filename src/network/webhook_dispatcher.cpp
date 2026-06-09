#include "network/webhook_dispatcher.hpp"
#include "network/http_client.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace xls20::net {

// ── Constructor ───────────────────────────────────────────────────────────────

WebhookDispatcher::WebhookDispatcher(boost::asio::io_context& ioc, std::string url)
    : ioc_(ioc)
    , url_(std::move(url))
{}

// ── dispatch ──────────────────────────────────────────────────────────────────

void WebhookDispatcher::dispatch(const std::string& collection_name,
                                 const std::string& signal_str,
                                 double             current_price,
                                 double             lower_band) {
    if (url_.empty()) return;

    // ── ISO 8601 UTC timestamp ─────────────────────────────────────────────
    auto now   = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now);
    char ts_buf[32];
    std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%SZ",
                  std::gmtime(&now_t));

    // ── Standardized JSON payload ──────────────────────────────────────────
    const nlohmann::json payload = {
        {"event",         "XLS20_PREDICTIVE_SIGNAL"},
        {"collection",    collection_name},
        {"signal",        signal_str},
        {"current_price", current_price},
        {"lower_band",    lower_band},
        {"timestamp",     std::string(ts_buf)}
    };

    const std::string body = payload.dump();

    // ── Fire-and-forget async POST ─────────────────────────────────────────
    // Capture collection_name and signal_str by value for the completion log.
    // The lambda owns the copies — no dangling references possible.
    async_http_post(ioc_, url_, body,
        [coll = collection_name, sig = signal_str]
        (bool ok, unsigned status, std::string /*body*/) {
            if (ok && status >= 200 && status < 300) {
                std::cout << "[Webhook] " << sig << " alert dispatched — "
                          << coll << " (HTTP " << status << ")\n";
            } else {
                std::cerr << "[Webhook] Alert delivery failed — "
                          << coll << "  ok=" << std::boolalpha << ok
                          << "  status=" << status << '\n';
            }
        });
}

} // namespace xls20::net
