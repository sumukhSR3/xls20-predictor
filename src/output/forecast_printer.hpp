#pragma once

#include <string>
#include <vector>
#include <ostream>

#include "registry/collection.hpp"

namespace xls20::output {

/**
 * @brief Renders periodic forecast summaries to an ANSI terminal.
 *
 * Features:
 *  - Full-width bordered table (adapts to terminal width)
 *  - Per-collection sparkline built from the last N bucket closes
 *  - 1-hour, 6-hour, and 24-hour XRP price forecasts
 *  - Trend arrow  (↑ green | ↓ red | → yellow)
 *  - MAE / RMSE error columns
 *  - Trade and bucket counts
 *  - Phase 4: SIGNAL column (BUY / SELL / HOLD) with band width display
 */
class ForecastPrinter {
public:
    struct Options {
        int    sparkline_bars{20};   ///< Characters in the sparkline
        bool   use_color{true};      ///< Emit ANSI escape codes
        int    forecast_horizon{24}; ///< Hours shown in the forecast table
        std::ostream* out{nullptr};  ///< nullptr → stdout
    };

    ForecastPrinter() : ForecastPrinter(Options{}) {}
    explicit ForecastPrinter(Options opts);

    /**
     * @brief Print a full forecast table for all supplied collections.
     *
     * Clears the terminal, prints a timestamped header, then one row
     * per collection.
     */
    void print(const std::vector<ForecastResult>& forecasts) const;

private:
    // ── Rendering helpers ─────────────────────────────────────────────────────
    std::string sparkline(const std::vector<double>& values) const;
    std::string trend_arrow(int direction) const;
    std::string color(const char* ansi_code, const std::string& text) const;
    std::string fmt_xrp(double xrp) const;

    /// Phase 4: render the SIGNAL cell with appropriate color coding.
    std::string signal_label(TradingSignal sig) const;

    Options opts_;
};

} // namespace xls20::output
