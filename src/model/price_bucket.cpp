#include "model/price_bucket.hpp"

#include <algorithm>
#include <cmath>

namespace xls20::model {

// ── PriceBucket helpers ───────────────────────────────────────────────────────

double PriceBucket::vwap() const noexcept {
    // volume_xrp is the sum of individual trade prices (not quantity-weighted),
    // so dividing by trade_count gives a simple arithmetic mean price.
    return (trade_count > 0) ? (volume_xrp / static_cast<double>(trade_count))
                             : close;
}

double PriceBucket::mid() const noexcept {
    return (high + low) * 0.5;
}

// ── BucketAccumulator ─────────────────────────────────────────────────────────

BucketAccumulator::BucketAccumulator(uint64_t bucket_duration_seconds)
    : bucket_dur_(bucket_duration_seconds)
{}

std::optional<PriceBucket> BucketAccumulator::add_sale(double   price_xrp,
                                                       uint64_t timestamp) {
    // Align timestamp to the start of its bucket
    uint64_t bucket_start = (timestamp / bucket_dur_) * bucket_dur_;

    // ── First-ever trade: open a fresh bucket ─────────────────────────────────
    if (current_start_ == 0) {
        current_start_       = bucket_start;
        current_.timestamp   = bucket_start;
        current_.open        = price_xrp;
        current_.high        = price_xrp;
        current_.low         = price_xrp;
        current_.close       = price_xrp;
        current_.volume_xrp  = price_xrp;
        current_.trade_count = 1;
        current_.valid       = true;
        return std::nullopt;
    }

    // ── Same bucket: accumulate ───────────────────────────────────────────────
    if (bucket_start == current_start_) {
        current_.high         = std::max(current_.high, price_xrp);
        current_.low          = std::min(current_.low,  price_xrp);
        current_.close        = price_xrp;
        current_.volume_xrp  += price_xrp;
        current_.trade_count++;
        return std::nullopt;
    }

    // ── New bucket boundary: seal the old bucket, open a new one ─────────────
    PriceBucket closed  = current_;      // return by value

    current_            = PriceBucket{};
    current_start_      = bucket_start;
    current_.timestamp  = bucket_start;
    current_.open       = price_xrp;
    current_.high       = price_xrp;
    current_.low        = price_xrp;
    current_.close      = price_xrp;
    current_.volume_xrp = price_xrp;
    current_.trade_count = 1;
    current_.valid      = true;

    return closed;
}

std::optional<PriceBucket> BucketAccumulator::close_current() {
    if (!current_.valid) return std::nullopt;
    PriceBucket closed = current_;
    current_           = PriceBucket{};
    current_start_     = 0;
    return closed;
}

} // namespace xls20::model
