#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "model/price_bucket.hpp"
#include "model/holt_winters.hpp"

namespace xls20 {

// ── TradingSignal ─────────────────────────────────────────────────────────────

/**
 * @brief Directional signal derived from the adaptive volatility bands.
 *
 * BUY  — current price has pierced the lower 95 % confidence band (oversold).
 * SELL — current price has pierced the upper 95 % confidence band (overbought).
 * HOLD — price is within the bands (no clear edge).
 */
enum class TradingSignal : int8_t {
    HOLD =  0,
    BUY  =  1,
    SELL = -1,
};

// ── ForecastResult ────────────────────────────────────────────────────────────

/**
 * @brief Snapshot of a collection's current state and price forecasts.
 *
 * Produced by CollectionRegistry and consumed by ForecastPrinter.
 *
 * Phase 4 additions:
 *   upper_band / lower_band — the first-step 95 % CI boundary from
 *   HoltWinters::ForecastEnvelope (same z multiplier, 1.96 default).
 *   band_width              — z * rolling_rmse (constant across the horizon).
 *   signal                  — BUY / SELL / HOLD derived from current_price vs bands.
 */
struct ForecastResult {
    std::string          collection_name;
    std::string          collection_key;   ///< "<issuer_id>:<taxon>"
    double               current_price_xrp{0.0}; ///< Most recent bucket close
    std::vector<double>  predictions;             ///< Next N hourly point predictions
    std::vector<double>  upper_band;              ///< Phase 4: upper confidence boundaries
    std::vector<double>  lower_band;              ///< Phase 4: lower confidence boundaries
    double               band_width{0.0};         ///< Phase 4: z * rolling_rmse
    TradingSignal        signal{TradingSignal::HOLD}; ///< Phase 4: derived trading signal
    double               mae{0.0};
    double               rmse{0.0};
    int                  trend_direction{0};  ///< +1 up | -1 down | 0 flat
    size_t               total_trades{0};
    size_t               bucket_count{0};
};

// ── Collection ────────────────────────────────────────────────────────────────

/**
 * @brief Aggregates all per-collection state: config, trade history,
 *        price accumulator, forecaster, and the latest forecast.
 *
 * Owned by CollectionRegistry; not shared between threads without a mutex.
 */
struct Collection {
    // ── Config (immutable after construction) ─────────────────────────────────
    std::string name;
    std::string issuer;       ///< 40-char hex AccountID
    uint32_t    taxon{0};
    std::string description;

    // ── Runtime state ─────────────────────────────────────────────────────────
    model::BucketAccumulator accumulator;
    model::HoltWinters       forecaster;
    std::vector<model::PriceBucket> completed_buckets;
    ForecastResult           last_forecast;
    size_t                   total_trades{0};

    // Phase 5: tracks the last signal that fired a webhook POST.
    // Initialised to HOLD so the very first non-neutral signal always
    // triggers a dispatch; subsequent identical signals are suppressed.
    TradingSignal            last_dispatched_signal{TradingSignal::HOLD};

    // ── Helpers ───────────────────────────────────────────────────────────────
    /// Returns "<issuer>:<taxon>" — the key used in maps and NFTSale structs.
    [[nodiscard]]
    std::string collection_key() const {
        return issuer + ":" + std::to_string(taxon);
    }

    // ── Constructor ───────────────────────────────────────────────────────────
    Collection(std::string n, std::string iss, uint32_t tax,
               std::string desc,
               uint64_t              bucket_duration_s,
               const model::HoltWintersParams& hw_params)
        : name(std::move(n))
        , issuer(std::move(iss))
        , taxon(tax)
        , description(std::move(desc))
        , accumulator(bucket_duration_s)
        , forecaster(hw_params)
    {}
};

} // namespace xls20
