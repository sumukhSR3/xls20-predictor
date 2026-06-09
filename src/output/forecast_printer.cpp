#include "output/forecast_printer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace xls20::output {

// ── ANSI palette ──────────────────────────────────────────────────────────────
namespace ansi {
    static constexpr const char* Reset    = "\033[0m";
    static constexpr const char* Bold     = "\033[1m";
    static constexpr const char* Dim      = "\033[2m";
    static constexpr const char* Green    = "\033[32m";
    static constexpr const char* Red      = "\033[31m";
    static constexpr const char* Yellow   = "\033[33m";
    static constexpr const char* Cyan     = "\033[36m";
    static constexpr const char* Magenta  = "\033[35m";
    static constexpr const char* White    = "\033[97m";
    static constexpr const char* Gray     = "\033[90m";
    static constexpr const char* BGBlue   = "\033[44m";
    static constexpr const char* ClearScr = "\033[2J\033[H";

    // Composed bold+color sequences for the SIGNAL column
    static constexpr const char* BoldGreen = "\033[1;32m";
    static constexpr const char* BoldRed   = "\033[1;31m";
}

// Braille-style 8-level sparkline blocks
static constexpr const char* kSparkChars[] = {
    "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"
};

// ── Constructor ───────────────────────────────────────────────────────────────

ForecastPrinter::ForecastPrinter(Options opts)
    : opts_(std::move(opts))
{
    if (!opts_.out) opts_.out = &std::cout;
}

// ── Helpers ───────────────────────────────────────────────────────────────────

std::string ForecastPrinter::color(const char* ansi_code,
                                   const std::string& text) const {
    if (!opts_.use_color) return text;
    return std::string(ansi_code) + text + ansi::Reset;
}

std::string ForecastPrinter::fmt_xrp(double xrp) const {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4) << xrp << " XRP";
    return ss.str();
}

std::string ForecastPrinter::trend_arrow(int direction) const {
    switch (direction) {
        case  1: return color(ansi::Green,  "  ↑ UP  ");
        case -1: return color(ansi::Red,    "  ↓ DOWN");
        default: return color(ansi::Yellow, "  → FLAT");
    }
}

// ── Phase 4: signal_label ─────────────────────────────────────────────────────
//
// Returns a fixed-width (6 visible chars) SIGNAL label with ANSI color:
//   BUY  → bold green   │ price broke below the lower 95 % CI band (oversold)
//   SELL → bold red     │ price broke above the upper 95 % CI band (overbought)
//   HOLD → dim gray     │ price inside the bands (no edge)
//
// A fixed-width cell ensures the table stays perfectly aligned regardless of
// which signal fires.  The visible cell is " BUY  ", " SELL ", " HOLD " —
// each exactly 6 characters — padded to align with the column header.

std::string ForecastPrinter::signal_label(TradingSignal sig) const {
    switch (sig) {
        case TradingSignal::BUY:
            return color(ansi::BoldGreen, " BUY  ");
        case TradingSignal::SELL:
            return color(ansi::BoldRed,   " SELL ");
        default:
            return color(ansi::Gray,      " HOLD ");
    }
}

std::string ForecastPrinter::sparkline(const std::vector<double>& values) const {
    if (values.empty()) return std::string(
        static_cast<size_t>(opts_.sparkline_bars), ' ');

    int bars = opts_.sparkline_bars;
    // Use the last N values
    int start = std::max(0, static_cast<int>(values.size()) - bars);
    std::vector<double> slice(values.begin() + start, values.end());

    double mn = *std::min_element(slice.begin(), slice.end());
    double mx = *std::max_element(slice.begin(), slice.end());
    double rng = mx - mn;

    std::string line;
    for (double v : slice) {
        int idx = (rng < 1e-12)
            ? 4
            : static_cast<int>(std::round(7.0 * (v - mn) / rng));
        idx = std::clamp(idx, 0, 7);
        line += kSparkChars[idx];
    }
    // Pad to full width
    int missing = bars - static_cast<int>(slice.size());
    for (int i = 0; i < missing; ++i) line = " " + line;
    return line;
}

// ── Main print routine ────────────────────────────────────────────────────────

void ForecastPrinter::print(const std::vector<ForecastResult>& forecasts) const {
    std::ostream& out = *opts_.out;

    // Clear screen + move cursor to top
    if (opts_.use_color) out << ansi::ClearScr;

    // ── Header ────────────────────────────────────────────────────────────────
    auto now    = std::chrono::system_clock::now();
    auto now_t  = std::chrono::system_clock::to_time_t(now);
    char ts_buf[32];
    std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%d %H:%M:%S UTC",
                  std::gmtime(&now_t));

    out << color(ansi::Bold,
         color(ansi::Cyan,
         "╔══════════════════════════════════════════════════════════════════════════╗\n"))
        << color(ansi::Bold,
         color(ansi::Cyan,
         "║  XLS-20 NFT Price Predictor  •  Holt-Winters v1.0  •  Phase 4 Signals  ║\n"))
        << color(ansi::Bold,
         color(ansi::Cyan,
         "╚══════════════════════════════════════════════════════════════════════════╝\n"));
    out << color(ansi::Dim, "  Updated: ")
        << color(ansi::White, ts_buf) << "\n\n";

    if (forecasts.empty()) {
        out << color(ansi::Yellow,
            "  Waiting for transaction data…  "
            "(subscribed to XRPL transaction stream)\n");
        return;
    }

    // ── Column header ─────────────────────────────────────────────────────────
    //
    // Layout (each column's visible width in chars):
    //   Collection  19 │ Trend  8 │ SIGNAL  7 │ Current  12 │ +1h  12 │
    //   +6h  12 │ +24h  12 │ MAE  8 │ Trades  6
    //
    out << color(ansi::Bold, color(ansi::Magenta,
         "  Collection          Trend    SIGNAL   Current      +1h          +6h"
         "          +24h         MAE      Trades\n"))
        << color(ansi::Dim,
         "  ─────────────────── ──────── ──────── ──────────── ──────────── "
         "──────────── ──────────── ──────── ──────\n");

    // ── Rows ──────────────────────────────────────────────────────────────────
    for (const auto& fr : forecasts) {
        // Collection name (truncated to 19 chars)
        std::string name = fr.collection_name;
        if (name.size() > 19) name = name.substr(0, 16) + "…";
        name.resize(19, ' ');

        // Trend arrow
        std::string trend = trend_arrow(fr.trend_direction);

        // Phase 4: SIGNAL cell
        std::string sig_cell = signal_label(fr.signal);

        // Prices
        auto get_pred = [&](size_t idx) -> std::string {
            if (fr.predictions.size() > idx)
                return fmt_xrp(fr.predictions[idx]);
            return "    —     ";
        };

        std::string curr = fmt_xrp(fr.current_price_xrp);
        std::string p1h  = get_pred(0);
        std::string p6h  = get_pred(5);
        std::string p24h = get_pred(23);

        // MAE
        std::ostringstream mae_ss;
        mae_ss << std::fixed << std::setprecision(4) << fr.mae;

        // Color the +24h prediction by trend direction
        const char* price_color = (fr.trend_direction >  0) ? ansi::Green
                                : (fr.trend_direction <  0) ? ansi::Red
                                                           : ansi::White;

        out << "  " << name << " "
            << trend       << "  "
            << sig_cell    << "  "
            << color(ansi::White,   std::string(12, ' ').replace(0, curr.size(), curr)) << " "
            << color(ansi::Cyan,    std::string(12, ' ').replace(0, p1h.size(),  p1h))  << " "
            << color(ansi::Cyan,    std::string(12, ' ').replace(0, p6h.size(),  p6h))  << " "
            << color(price_color,   std::string(12, ' ').replace(0, p24h.size(), p24h)) << " "
            << color(ansi::Dim,     std::string(8, ' ').replace(0, mae_ss.str().size(), mae_ss.str())) << " "
            << color(ansi::Yellow,  std::to_string(fr.total_trades))
            << "\n";

        // ── Phase 4: Confidence-band sub-row ─────────────────────────────────
        //
        // Display the first-step lower / upper boundary and the band half-width
        // (z * RMSE) so the operator can see how wide the CI is in XRP terms.
        // If bands are not yet populated (model not fitted) we omit the row.
        if (!fr.lower_band.empty() && !fr.upper_band.empty()) {
            std::ostringstream band_ss;
            band_ss << std::fixed << std::setprecision(4);
            band_ss << "  Bands [" << fr.lower_band.front()
                    << " XRP … "   << fr.upper_band.front()
                    << " XRP]  ±"  << fr.band_width << " XRP";

            out << color(ansi::Dim, "                        " + band_ss.str())
                << "\n";
        }

        // ── Sparkline sub-row ─────────────────────────────────────────────────
        std::vector<double> spark_data;
        spark_data.reserve(fr.predictions.size() + 1);
        if (fr.current_price_xrp > 0.0)
            spark_data.push_back(fr.current_price_xrp);
        for (double p : fr.predictions)
            spark_data.push_back(p);

        out << color(ansi::Dim, "                        Forecast: ")
            << color(price_color, sparkline(spark_data))
            << "\n";
    }

    out << color(ansi::Dim,
         "\n  ──────────────────────────────────────────────────────────────────────\n")
        << color(ansi::Dim,
         "  Model: Additive Holt-Winters  |  Bands: 1.96 × RMSE (95 % CI)"
         "  |  Source: xrplcluster.com\n")
        << std::flush;
}

} // namespace xls20::output
