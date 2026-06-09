#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace xls20::model {

// ── PriceBucket ───────────────────────────────────────────────────────────────

/**
 * @brief OHLCV snapshot for a fixed-width time window.
 *
 * A bucket is "valid" once at least one trade has been recorded inside it.
 * The VWAP is computed as total volume divided by trade count; this is a
 * simple arithmetic approximation (not true value-weighted) unless the
 * volume field is updated with individual trade values.
 */
struct PriceBucket {
    uint64_t timestamp{0};    ///< Unix epoch of the bucket's start (seconds)
    double   open{0.0};
    double   high{0.0};
    double   low{0.0};
    double   close{0.0};
    double   volume_xrp{0.0};  ///< Cumulative XRP traded in this window
    uint32_t trade_count{0};
    bool     valid{false};

    /// Simple volume-weighted average price within the bucket.
    [[nodiscard]] double vwap() const noexcept;

    /// Mid-price: (high + low) / 2.
    [[nodiscard]] double mid() const noexcept;
};

// ── BucketAccumulator ─────────────────────────────────────────────────────────

/**
 * @brief Accumulates individual NFT sales into fixed-width OHLCV buckets.
 *
 * Every call to add_sale() may return a freshly closed PriceBucket if the
 * new trade crosses into the next time window.  Callers should append any
 * returned bucket to their historical series and feed it to the forecaster.
 *
 * Not thread-safe.
 */
class BucketAccumulator {
public:
    /// @param bucket_duration_seconds  Width of each time window (default 1 h).
    explicit BucketAccumulator(uint64_t bucket_duration_seconds = 3'600);

    /**
     * @brief Record a single NFT sale.
     * @param price_xrp  Sale price in XRP.
     * @param timestamp  Unix epoch seconds of the ledger close.
     * @return A completed PriceBucket if a window boundary was crossed;
     *         std::nullopt if the sale was absorbed into the current window.
     */
    [[nodiscard]]
    std::optional<PriceBucket> add_sale(double price_xrp, uint64_t timestamp);

    /// Forcibly close the current in-progress bucket (e.g. on shutdown).
    [[nodiscard]]
    std::optional<PriceBucket> close_current();

    [[nodiscard]] const PriceBucket& current()     const noexcept { return current_; }
    [[nodiscard]] uint64_t          bucket_duration() const noexcept { return bucket_dur_; }

private:
    uint64_t    bucket_dur_;
    uint64_t    current_start_{0};
    PriceBucket current_;
};

} // namespace xls20::model
