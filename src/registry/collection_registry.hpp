#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "parser/tx_filter.hpp"
#include "registry/collection.hpp"
#include "model/holt_winters.hpp"
#include <nlohmann/json.hpp>

// Forward declaration — avoids pulling <boost/beast.hpp> into every TU that
// includes this header.  The full definition is only needed in .cpp files that
// call set_webhook_dispatcher() or webhook_->dispatch().
namespace xls20::net { class WebhookDispatcher; }

namespace xls20 {

/**
 * @brief Manages all tracked NFT collections and orchestrates forecasting.
 *
 * Phase 2 additions:
 *  - Hot-reload: check_config_reload() compares the filesystem last-write-time
 *    of `config/collections.json` against the timestamp recorded at the last
 *    load.  If the file has changed, it parses the new contents and inserts
 *    *only new* collections into the live map — existing collections keep all
 *    their accumulated state (buckets, forecaster weights, trade history).
 *
 * Phase 3 additions:
 *  - Deep state persistence: save_cache() / load_cache() now serialise the
 *    full Holt-Winters mathematical state (level, trend, seasonal indices,
 *    fitted values, residuals, calibrated params) via HoltWinters::export_state()
 *    / import_state().  The cache format is bumped to v2.0.
 *  - Automatic checkpoint: every time a price bucket closes inside ingest(),
 *    save_cache_locked() flushes the updated state to disk immediately.  This
 *    caps data loss to at most one open (incomplete) bucket on a crash.
 *
 * Thread safety: all public methods are protected by a single mutex.  It is
 * safe to call ingest() from the ASIO thread, get_forecasts() from the printer
 * thread, and check_config_reload() from a third periodic-check thread.
 */
class CollectionRegistry {
public:
    struct Config {
        std::string config_path   = "config/collections.json";
        std::string cache_path    = "data/historical_cache.json";
        uint64_t    bucket_dur_s  = 3'600;  ///< Seconds per price bucket
        int         min_buckets   = 10;     ///< Min closed buckets before forecasting
        int         forecast_h    = 24;     ///< Periods ahead to forecast
        model::HoltWintersParams hw;
    };

    CollectionRegistry() : CollectionRegistry(Config{}) {}
    explicit CollectionRegistry(Config cfg);

    /// Load collection definitions from collections.json (initial startup).
    bool load_collections();

    /// Load historical bucket cache from disk (warm-start).
    bool load_cache();

    /// Persist the current bucket history to disk.
    bool save_cache() const;

    /**
     * @brief Ingest a confirmed NFT sale.
     *
     * Routes the sale to the matching collection, updates the accumulator,
     * closes buckets as needed, and triggers a re-forecast when enough
     * data is available.
     *
     * @return true if the sale was matched to a tracked collection.
     */
    bool ingest(const parser::NFTSale& sale);

    /// Return a snapshot of the latest forecast for every tracked collection.
    [[nodiscard]]
    std::vector<ForecastResult> get_forecasts() const;

    /// Number of tracked collections.
    [[nodiscard]] size_t collection_count() const;

    /**
     * @brief Hot-reload: compare config file mtime and inject new collections.
     *
     * Call this periodically (e.g. every 30 s) from any thread.  The method
     * is fully thread-safe.  It reads the file only when the mtime has advanced
     * since the last successful load, so normal iterations are O(1) stat calls.
     *
     * Existing collections are never removed or reset — their accumulated
     * price history, bucket state, and forecaster weights are preserved.
     * Only collections whose key (issuer:taxon) is not yet in the map are
     * inserted.
     *
     * @return Number of new collections injected (0 if no reload occurred).
     */
    int check_config_reload();

    // ── Phase 5: Multi-source ingestion & webhook alerting ────────────────────

    /**
     * @brief Attach a webhook dispatcher for outbound signal alerts.
     *
     * Must be called before the first ingest() — typically once from main()
     * after the registry is constructed but before ioc.run() starts.
     * Thread-safe (acquires mtx_).
     */
    void set_webhook_dispatcher(
        std::shared_ptr<net::WebhookDispatcher> dispatcher);

    /**
     * @brief Inject synthetic historical price buckets for cold-start seeding.
     *
     * Synthesises `PriceBucket` objects from a flat close-price array and
     * appends them to the named collection's completed_buckets, then triggers
     * a reforecast if the total bucket count reaches min_buckets.
     *
     * Only runs if the collection currently has fewer real buckets than
     * min_buckets — live data is never overwritten.
     *
     * @param collection_key     `"<issuer>:<taxon>"` map key.
     * @param closes             Oldest-first close-price array (XRP).
     * @param base_timestamp_s   Unix epoch seconds of the *most recent* bucket.
     * @param bucket_duration_s  Duration of each bucket in seconds (e.g. 3600).
     * @return Number of buckets actually injected (0 if collection unknown or
     *         already has sufficient data).
     */
    int ingest_historical(const std::string&         collection_key,
                          const std::vector<double>& closes,
                          uint64_t                   base_timestamp_s,
                          uint64_t                   bucket_duration_s);

private:
    // ── Internal helpers ──────────────────────────────────────────────────────
    void reforecast(Collection& col);
    void update_forecast_result(Collection& col);

    /// Parse the JSON document and inject new-only entries into collections_.
    /// Caller must hold mtx_.
    int inject_new_collections(const nlohmann::json& doc);

    /**
     * @brief Write the full cache to disk without acquiring mtx_.
     *
     * Must be called while the caller already holds mtx_.  Used internally
     * by save_cache() (which acquires the lock first) and by ingest() (which
     * already holds the lock) to flush a checkpoint every time a bucket closes.
     *
     * Format v2.0 — each collection entry is an object containing:
     *   "buckets"    : OHLCV array (same as v1)
     *   "hw_state"   : full Holt-Winters snapshot via export_state()
     *   "total_trades": uint64
     */
    bool save_cache_locked() const;

    // ── State ─────────────────────────────────────────────────────────────────
    Config                            cfg_;
    std::map<std::string, Collection> collections_;  ///< key = collection_key()
    mutable std::mutex                mtx_;

    /// Filesystem last-write-time at the last successful config load.
    std::filesystem::file_time_type   config_mtime_{};

    /// Phase 5: optional outbound webhook for BUY/SELL signal transitions.
    /// Null when no webhook URL was configured.
    std::shared_ptr<net::WebhookDispatcher> webhook_;
};

} // namespace xls20
