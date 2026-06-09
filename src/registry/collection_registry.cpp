#include "registry/collection_registry.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include <nlohmann/json.hpp>

// Phase 5: full WebhookDispatcher definition required to call dispatch().
#include "network/webhook_dispatcher.hpp"
#include "model/price_bucket.hpp"

namespace xls20 {

using json = nlohmann::json;
namespace fs = std::filesystem;

// ── Constructor ───────────────────────────────────────────────────────────────

CollectionRegistry::CollectionRegistry(Config cfg)
    : cfg_(std::move(cfg))
{}

// ── Internal: parse JSON → inject new-only collections ───────────────────────

int CollectionRegistry::inject_new_collections(const json& doc) {
    // Caller holds mtx_.
    if (!doc.contains("collections") || !doc["collections"].is_array())
        return 0;

    int added = 0;
    for (const auto& c : doc["collections"]) {
        std::string name   = c.value("name",        "Unknown");
        std::string issuer = c.value("issuer",       "");
        uint32_t    taxon  = c.value("taxon",        uint32_t{0});
        std::string desc   = c.value("description",  "");

        if (issuer.empty()) {
            std::cerr << "[Registry] Skipping collection with empty issuer: "
                      << name << '\n';
            continue;
        }
        // Normalise issuer to uppercase for consistent key comparisons.
        for (char& ch : issuer)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));

        Collection col(name, issuer, taxon, desc, cfg_.bucket_dur_s, cfg_.hw);
        std::string key = col.collection_key();

        if (collections_.count(key) == 0) {
            // Brand-new collection — insert with fresh state.
            collections_.emplace(key, std::move(col));
            std::cout << "[Registry] New collection tracked: " << name
                      << " (" << key << ")\n";
            ++added;
        }
        // Existing collections are deliberately left untouched.
    }
    return added;
}

// ── load_collections ──────────────────────────────────────────────────────────

bool CollectionRegistry::load_collections() {
    std::ifstream file(cfg_.config_path);
    if (!file) {
        std::cerr << "[Registry] Cannot open config: " << cfg_.config_path << '\n';
        return false;
    }

    json doc;
    try {
        file >> doc;
    } catch (const json::exception& e) {
        std::cerr << "[Registry] JSON parse error in config: " << e.what() << '\n';
        return false;
    }

    // Optional: override HW params from config (only on initial load).
    if (doc.contains("forecasting")) {
        const auto& f = doc["forecasting"];
        if (f.contains("alpha"))  cfg_.hw.alpha  = f["alpha"].get<double>();
        if (f.contains("beta"))   cfg_.hw.beta   = f["beta"].get<double>();
        if (f.contains("gamma"))  cfg_.hw.gamma  = f["gamma"].get<double>();
        if (f.contains("period")) cfg_.hw.period = f["period"].get<int>();
        if (f.contains("bucket_duration_seconds"))
            cfg_.bucket_dur_s = f["bucket_duration_seconds"].get<uint64_t>();
        if (f.contains("min_data_points"))
            cfg_.min_buckets = f["min_data_points"].get<int>();
        if (f.contains("forecast_horizon"))
            cfg_.forecast_h  = f["forecast_horizon"].get<int>();
    }

    std::lock_guard<std::mutex> lock(mtx_);
    int loaded = inject_new_collections(doc);

    if (loaded == 0) {
        std::cerr << "[Registry] No new collections found in config.\n";
        return false;
    }
    std::cout << "[Registry] Loaded " << loaded << " collection(s).\n";

    // Record the mtime so hot-reload can detect future changes.
    std::error_code mtime_ec;
    config_mtime_ = fs::last_write_time(cfg_.config_path, mtime_ec);

    return true;
}

// ── check_config_reload (Phase 2: hot-reload) ─────────────────────────────────

int CollectionRegistry::check_config_reload() {
    // ── 1. Stat the file — O(1), no lock needed for the filesystem call ───────
    std::error_code ec;
    auto mtime = fs::last_write_time(cfg_.config_path, ec);
    if (ec) {
        // File may be temporarily unavailable during an atomic write — ignore.
        return 0;
    }
    if (mtime == config_mtime_) {
        return 0;  // unchanged
    }

    // ── 2. File has changed — open and parse ──────────────────────────────────
    std::ifstream file(cfg_.config_path);
    if (!file) return 0;

    json doc;
    try {
        file >> doc;
    } catch (const json::exception& e) {
        std::cerr << "[Registry] Hot-reload parse error: " << e.what()
                  << " — keeping existing collections.\n";
        return 0;
    }

    // ── 3. Inject under lock — existing collections untouched ────────────────
    std::lock_guard<std::mutex> lock(mtx_);
    int added = inject_new_collections(doc);

    // Update the recorded mtime only after a successful parse.
    config_mtime_ = mtime;

    if (added > 0)
        std::cout << "[Registry] Hot-reload: injected " << added
                  << " new collection(s).\n";
    else
        std::cout << "[Registry] Hot-reload: config changed but no new collections.\n";

    return added;
}

// ── Phase 3: save_cache_locked (internal, non-locking writer) ─────────────────
//
// PRECONDITION: caller already holds mtx_.
//
// Writes the full v2.0 cache format:
//   { "version": "2.0", "generated_at": <unix-s>,
//     "collections": {
//       "<key>": {
//         "total_trades": <uint64>,
//         "buckets":  [ { ts, open, high, low, close, vol, trades }, ... ],
//         "hw_state": { <HoltWinters::export_state() snapshot> }
//       }
//     }
//   }
//
// Bucket closes happen at most once per hour per collection, so the
// lock-while-writing approach is intentional — the brief I/O window (~1 ms
// on NVMe) is negligible compared to the hourly inter-close interval.

bool CollectionRegistry::save_cache_locked() const {
    json doc;
    doc["version"]      = "2.0";
    doc["generated_at"] = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    for (const auto& [key, col] : collections_) {
        json entry;
        entry["total_trades"] = col.total_trades;

        // ── OHLCV bucket array ─────────────────────────────────────────────
        json buckets = json::array();
        for (const auto& b : col.completed_buckets) {
            buckets.push_back({
                {"ts",     b.timestamp},
                {"open",   b.open},
                {"high",   b.high},
                {"low",    b.low},
                {"close",  b.close},
                {"vol",    b.volume_xrp},
                {"trades", b.trade_count}
            });
        }
        entry["buckets"] = std::move(buckets);

        // ── Full Holt-Winters mathematical state ───────────────────────────
        // export_state() captures: params (α/β/γ/period), level, trend,
        // seasonals, fitted_values, residuals, fitted flag.
        // On warm-start, import_state() restores all of this directly
        // without requiring a re-run of fit().
        entry["hw_state"] = col.forecaster.export_state();

        doc["collections"][key] = std::move(entry);
    }

    std::ofstream file(cfg_.cache_path);
    if (!file) {
        std::cerr << "[Registry] Cannot write cache: " << cfg_.cache_path << '\n';
        return false;
    }
    file << doc.dump(2);
    return true;
}

// ── save_cache (public, acquires lock) ────────────────────────────────────────

bool CollectionRegistry::save_cache() const {
    std::lock_guard<std::mutex> lock(mtx_);
    bool ok = save_cache_locked();
    if (ok)
        std::cout << "[Registry] Cache written to " << cfg_.cache_path << '\n';
    return ok;
}

// ── load_cache ────────────────────────────────────────────────────────────────
//
// Handles both format versions:
//   v1: doc["collections"][key] is a bare JSON array of bucket objects.
//   v2: doc["collections"][key] is an object with "buckets", "hw_state",
//       and "total_trades" fields.
//
// After restoring buckets the Holt-Winters state is imported via
// import_state().  This means the forecaster is immediately usable for
// forecast() calls without requiring a re-run of fit(), and mae()/rmse()
// return the in-sample error metrics that were serialised at checkpoint time.

bool CollectionRegistry::load_cache() {
    std::ifstream file(cfg_.cache_path);
    if (!file) return false;   // no cache yet — not an error

    json doc;
    try {
        file >> doc;
    } catch (...) {
        std::cerr << "[Registry] Could not parse cache file — starting fresh.\n";
        return false;
    }

    if (!doc.contains("collections")) return false;

    const bool is_v2 = (doc.value("version", "1.0") == "2.0");

    std::lock_guard<std::mutex> lock(mtx_);
    int restored = 0;

    for (auto& [col_key, val] : doc["collections"].items()) {
        auto it = collections_.find(col_key);
        if (it == collections_.end()) continue;

        Collection& col = it->second;

        // ── Determine bucket source depending on format version ────────────
        const json* buckets_ptr = nullptr;
        if (is_v2 && val.is_object() && val.contains("buckets")) {
            buckets_ptr = &val["buckets"];
        } else if (val.is_array()) {
            // v1: bare array
            buckets_ptr = &val;
        }

        if (buckets_ptr) {
            for (const auto& b : *buckets_ptr) {
                model::PriceBucket pb;
                pb.timestamp   = b.value("ts",     uint64_t{0});
                pb.open        = b.value("open",    0.0);
                pb.high        = b.value("high",    0.0);
                pb.low         = b.value("low",     0.0);
                pb.close       = b.value("close",   0.0);
                pb.volume_xrp  = b.value("vol",     0.0);
                pb.trade_count = b.value("trades",  uint32_t{0});
                pb.valid       = true;
                col.completed_buckets.push_back(pb);
            }
        }

        // ── Restore total_trades ───────────────────────────────────────────
        if (is_v2 && val.contains("total_trades"))
            col.total_trades = val["total_trades"].get<uint64_t>();
        else
            col.total_trades = col.completed_buckets.size();  // v1 approximation

        // ── Restore Holt-Winters mathematical state (v2 only) ─────────────
        // import_state() restores: calibrated params, level, trend,
        // seasonal indices, fitted_values, residuals, and the fitted flag.
        // After this call the model is ready for forecast() and the
        // error metrics (mae/rmse) reflect the serialised in-sample fit.
        if (is_v2 && val.contains("hw_state")) {
            try {
                col.forecaster.import_state(val["hw_state"]);
            } catch (const std::exception& e) {
                std::cerr << "[Registry] hw_state import failed for " << col_key
                          << ": " << e.what() << " — will re-fit on next bucket.\n";
            }
        }

        ++restored;
    }

    std::cout << "[Registry] Restored " << restored
              << " collection(s) from cache (format v"
              << (is_v2 ? "2.0" : "1.0") << ").\n";
    return restored > 0;
}

// ── ingest ────────────────────────────────────────────────────────────────────

bool CollectionRegistry::ingest(const parser::NFTSale& sale) {
    std::lock_guard<std::mutex> lock(mtx_);

    auto it = collections_.find(sale.collection_key);
    if (it == collections_.end()) return false;

    Collection& col = it->second;
    col.total_trades++;

    auto closed = col.accumulator.add_sale(sale.price_xrp, sale.ledger_time);
    if (closed.has_value()) {
        col.completed_buckets.push_back(*closed);

        if (static_cast<int>(col.completed_buckets.size()) >= cfg_.min_buckets) {
            reforecast(col);
        }

        // ── Phase 3: Automatic checkpoint on every bucket close ────────────
        // save_cache_locked() runs under the mutex already held here.
        // Bucket closes happen at most once per hour, so the I/O cost is
        // amortised over thousands of ingest() calls and is negligible.
        save_cache_locked();
    }

    // Always reflect the latest live price.
    col.last_forecast.current_price_xrp = col.accumulator.current().close;
    col.last_forecast.total_trades       = col.total_trades;
    col.last_forecast.bucket_count       = col.completed_buckets.size();

    return true;
}

// ── reforecast ────────────────────────────────────────────────────────────────

void CollectionRegistry::reforecast(Collection& col) {
    std::vector<double> series;
    series.reserve(col.completed_buckets.size());
    for (const auto& b : col.completed_buckets)
        series.push_back(b.close);

    if (series.size() < 2 * col.forecaster.seasonals().size() &&
        !col.forecaster.is_fitted()) {
        double avg = 0.0;
        for (double v : series) avg += v;
        avg /= static_cast<double>(series.size());

        ForecastResult& fr = col.last_forecast;
        fr.collection_name = col.name;
        fr.collection_key  = col.collection_key();
        fr.predictions.assign(static_cast<size_t>(cfg_.forecast_h), avg);
        fr.trend_direction = 0;
        return;
    }

    try {
        col.forecaster.fit(series);
        update_forecast_result(col);
    } catch (const std::exception& e) {
        std::cerr << "[Registry] Forecast error for " << col.name
                  << ": " << e.what() << '\n';
    }
}

void CollectionRegistry::update_forecast_result(Collection& col) {
    ForecastResult& fr = col.last_forecast;
    fr.collection_name = col.name;
    fr.collection_key  = col.collection_key();
    fr.mae             = col.forecaster.mae();
    fr.rmse            = col.forecaster.rmse();
    fr.bucket_count    = col.completed_buckets.size();
    fr.total_trades    = col.total_trades;

    // ── Phase 4: populate envelope fields ────────────────────────────────────
    // forecast_envelope() returns point predictions + symmetric 95 % CI bands
    // computed from the rolling in-sample RMSE (z = 1.96 by default).
    const auto env = col.forecaster.forecast_envelope(cfg_.forecast_h);
    fr.predictions = env.point_forecast;
    fr.upper_band  = env.upper_band;
    fr.lower_band  = env.lower_band;
    fr.band_width  = env.band_width;

    // ── Derive trading signal ─────────────────────────────────────────────────
    // Compare the *current live price* against the first-step CI boundaries.
    // A price break below the lower band signals the market is statistically
    // cheap (BUY); above the upper band signals expensive (SELL).
    const double current = col.accumulator.current().close;
    if (!fr.upper_band.empty() && !fr.lower_band.empty() && current > 0.0) {
        if (current < fr.lower_band.front())
            fr.signal = TradingSignal::BUY;
        else if (current > fr.upper_band.front())
            fr.signal = TradingSignal::SELL;
        else
            fr.signal = TradingSignal::HOLD;
    } else {
        fr.signal = TradingSignal::HOLD;
    }

    // ── Trend direction (unchanged) ───────────────────────────────────────────
    if (!fr.predictions.empty()) {
        double diff = fr.predictions.front() - current;
        fr.trend_direction = (diff >  0.005 * current) ?  1
                           : (diff < -0.005 * current) ? -1
                                                       :  0;
    }

    // ── Phase 5: outbound webhook on signal transitions ─────────────────────
    //
    // Only fire a webhook when the signal *changes* from the last dispatched
    // value, preventing a sustained BUY from blasting the endpoint every hour.
    // When the signal returns to HOLD we reset the dedup state so the next
    // non-neutral transition will fire again.
    if (webhook_ && webhook_->enabled()) {
        if (fr.signal != TradingSignal::HOLD &&
            fr.signal != col.last_dispatched_signal) {
            const char* sig_str =
                (fr.signal == TradingSignal::BUY) ? "BUY" : "SELL";
            const double lb = fr.lower_band.empty() ? 0.0
                                                    : fr.lower_band.front();
            webhook_->dispatch(fr.collection_name, sig_str, current, lb);
            col.last_dispatched_signal = fr.signal;
        } else if (fr.signal == TradingSignal::HOLD) {
            // Reset dedup so the next transition fires.
            col.last_dispatched_signal = TradingSignal::HOLD;
        }
    }
}

// ── get_forecasts ─────────────────────────────────────────────────────────────

std::vector<ForecastResult> CollectionRegistry::get_forecasts() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<ForecastResult> results;
    results.reserve(collections_.size());
    for (const auto& [key, col] : collections_)
        results.push_back(col.last_forecast);
    return results;
}

size_t CollectionRegistry::collection_count() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return collections_.size();
}

// ── Phase 5: set_webhook_dispatcher ───────────────────────────────────────

void CollectionRegistry::set_webhook_dispatcher(
    std::shared_ptr<net::WebhookDispatcher> dispatcher) {
    std::lock_guard<std::mutex> lock(mtx_);
    webhook_ = std::move(dispatcher);
    if (webhook_ && webhook_->enabled())
        std::cout << "[Registry] Webhook dispatcher attached.\n";
}

// ── Phase 5: ingest_historical ───────────────────────────────────────────
//
// Synthesises PriceBucket objects from a bare close-price array and appends
// them to the named collection's completed_buckets, then triggers reforecast.
//
// OHLCV fields are approximated as open = high = low = close (we only have
// the close price from a typical historical candlestick endpoint), volume = 0,
// trade_count = 0.  The Holt-Winters model only uses the close column.
//
// Timestamps are assigned as:
//   closes[i].ts = base_timestamp_s - (n - 1 - i) * bucket_duration_s
// so closes[0] is the oldest bucket and closes[n-1] is the most recent one,
// with closes[n-1].ts == base_timestamp_s.

int CollectionRegistry::ingest_historical(
    const std::string&         collection_key,
    const std::vector<double>& closes,
    uint64_t                   base_timestamp_s,
    uint64_t                   bucket_duration_s) {

    if (closes.empty()) return 0;

    std::lock_guard<std::mutex> lock(mtx_);

    auto it = collections_.find(collection_key);
    if (it == collections_.end()) {
        std::cerr << "[Registry] ingest_historical: unknown key "
                  << collection_key << '\n';
        return 0;
    }

    Collection& col = it->second;

    // Guard: skip if the collection already has sufficient live data so we
    // never overwrite real OHLCV buckets with synthetic approximations.
    if (static_cast<int>(col.completed_buckets.size()) >= cfg_.min_buckets) {
        std::cout << "[Registry] " << collection_key
                  << " already has " << col.completed_buckets.size()
                  << " buckets — skipping historical seed.\n";
        return 0;
    }

    const size_t n = closes.size();
    int injected   = 0;

    for (size_t i = 0; i < n; ++i) {
        model::PriceBucket pb;
        // Assign timestamps oldest-first (closes[0] is the oldest).
        pb.timestamp   = base_timestamp_s
                       - static_cast<uint64_t>(n - 1 - i) * bucket_duration_s;
        pb.open        = closes[i];
        pb.high        = closes[i];
        pb.low         = closes[i];
        pb.close       = closes[i];
        pb.volume_xrp  = 0.0;
        pb.trade_count = 0;
        pb.valid       = true;
        col.completed_buckets.push_back(pb);
        ++injected;
    }

    std::cout << "[Registry] Seeded " << injected
              << " historical buckets for " << collection_key << '.\n';

    // Trigger a reforecast now that we have data.  reforecast() checks
    // internally whether there are enough buckets for HW initialisation.
    if (static_cast<int>(col.completed_buckets.size()) >= cfg_.min_buckets) {
        reforecast(col);
    }

    return injected;
}

} // namespace xls20
