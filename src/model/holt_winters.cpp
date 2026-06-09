#include "model/holt_winters.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace xls20::model {

// ── Constructor ───────────────────────────────────────────────────────────────

HoltWinters::HoltWinters(HoltWintersParams params)
    : params_(params)
{}

// ── Initialisation ────────────────────────────────────────────────────────────

/*
 * Classical Hyndman et al. initialisation for additive Holt-Winters:
 *
 *  L₀  = mean of the first m observations
 *  B₀  = (mean of second season − mean of first season) / m
 *  S[i] = y[i] − L₀    for i = 0 … m-1
 *
 * Two full seasons (2m observations) are required.
 */
void HoltWinters::initialise(const std::vector<double>& series) {
    const int m = params_.period;

    // Season 1 mean
    double sum1 = 0.0;
    for (int i = 0; i < m; ++i) sum1 += series[static_cast<size_t>(i)];
    double L0 = sum1 / m;

    // Season 2 mean
    double sum2 = 0.0;
    for (int i = m; i < 2 * m; ++i) sum2 += series[static_cast<size_t>(i)];
    double L1 = sum2 / m;

    level_ = L0;
    trend_ = (L1 - L0) / static_cast<double>(m);

    // Seasonal indices: deviation from initial level
    seasonals_.resize(static_cast<size_t>(m));
    for (int i = 0; i < m; ++i) {
        seasonals_[static_cast<size_t>(i)] = series[static_cast<size_t>(i)] - L0;
    }
}

// ── fit ───────────────────────────────────────────────────────────────────────

void HoltWinters::fit(const std::vector<double>& series) {
    const int m = params_.period;
    const int n = static_cast<int>(series.size());

    if (n < 2 * m) {
        throw std::invalid_argument(
            "HoltWinters::fit requires at least 2 × period (" +
            std::to_string(2 * m) + ") observations; got " +
            std::to_string(n));
    }

    initialise(series);

    fitted_values_.clear();
    fitted_values_.reserve(static_cast<size_t>(n));
    residuals_.clear();
    residuals_.reserve(static_cast<size_t>(n));

    double L = level_;
    double B = trend_;

    for (int t = 0; t < n; ++t) {
        const int    s_idx = t % m;        // seasonal index (circular)
        const double y     = series[static_cast<size_t>(t)];
        const double S_tm  = seasonals_[static_cast<size_t>(s_idx)];

        // One-step-ahead forecast (before updating state)
        double y_hat = L + B + S_tm;
        fitted_values_.push_back(y_hat);
        residuals_.push_back(y - y_hat);

        // Update state
        double L_prev = L;
        L = params_.alpha * (y - S_tm)
          + (1.0 - params_.alpha) * (L + B);
        B = params_.beta  * (L - L_prev)
          + (1.0 - params_.beta)  * B;
        seasonals_[static_cast<size_t>(s_idx)] =
            params_.gamma * (y - L_prev - B)
          + (1.0 - params_.gamma) * S_tm;
    }

    // Store final state for forecasting
    level_  = L;
    trend_  = B;
    fitted_ = true;
}

// ── forecast ──────────────────────────────────────────────────────────────────

std::vector<double> HoltWinters::forecast(int h) const {
    if (!fitted_)
        throw std::runtime_error("HoltWinters::forecast called before fit()");
    if (h <= 0)
        return {};

    const int n_seasonal = static_cast<int>(seasonals_.size()); // == params_.period

    std::vector<double> preds;
    preds.reserve(static_cast<size_t>(h));

    for (int step = 1; step <= h; ++step) {
        // Seasonal index cycles over the stored seasonal components
        int s_idx = ((static_cast<int>(fitted_values_.size()) + step - 1) % n_seasonal);
        double f  = level_
                  + static_cast<double>(step) * trend_
                  + seasonals_[static_cast<size_t>(s_idx)];
        preds.push_back(std::max(0.0, f));  // prices can't be negative
    }
    return preds;
}

// ── Phase 4: forecast_envelope ────────────────────────────────────────────────

HoltWinters::ForecastEnvelope HoltWinters::forecast_envelope(int h, double z) const {
    if (!fitted_)
        throw std::runtime_error("HoltWinters::forecast_envelope called before fit()");

    // Point forecasts — reuse the validated forecast() implementation.
    ForecastEnvelope env;
    env.point_forecast = forecast(h);

    // Rolling RMSE from in-sample residuals.
    // rmse() guards against empty residuals_ (returns 0.0), so no extra check needed.
    const double sigma    = rmse();
    const double half_width = z * sigma;
    env.band_width = half_width;

    const int n = static_cast<int>(env.point_forecast.size());
    env.upper_band.reserve(static_cast<size_t>(n));
    env.lower_band.reserve(static_cast<size_t>(n));

    for (int i = 0; i < n; ++i) {
        const double pt = env.point_forecast[static_cast<size_t>(i)];
        env.upper_band.push_back(pt + half_width);
        // Prices cannot be negative — clamp lower band at zero.
        env.lower_band.push_back(std::max(0.0, pt - half_width));
    }

    return env;
}

// ── Error metrics ─────────────────────────────────────────────────────────────

double HoltWinters::mae() const {
    if (residuals_.empty()) return 0.0;
    double sum = 0.0;
    for (double r : residuals_) sum += std::abs(r);
    return sum / static_cast<double>(residuals_.size());
}

double HoltWinters::rmse() const {
    if (residuals_.empty()) return 0.0;
    double sum = 0.0;
    for (double r : residuals_) sum += r * r;
    return std::sqrt(sum / static_cast<double>(residuals_.size()));
}

// ── Phase 3: State serialization ────────────────────────────────────────────

nlohmann::json HoltWinters::export_state() const {
    using json = nlohmann::json;
    return json{
        {"params", {
            {"alpha",  params_.alpha},
            {"beta",   params_.beta},
            {"gamma",  params_.gamma},
            {"period", params_.period}
        }},
        {"level",         level_},
        {"trend",         trend_},
        {"seasonals",     seasonals_},
        // fitted_values and residuals are needed to restore mae()/rmse() exactly.
        // For large series these are non-trivial; they are compressed in the cache
        // file by JSON number precision, which is acceptable for an analytics tool.
        {"fitted_values", fitted_values_},
        {"residuals",     residuals_},
        {"fitted",        fitted_}
    };
}

void HoltWinters::import_state(const nlohmann::json& s) {
    // Params — restore calibrated smoothing factors before everything else
    // so that any subsequent fit() call uses the right values.
    if (s.contains("params")) {
        const auto& p = s["params"];
        if (p.contains("alpha"))  params_.alpha  = p["alpha"].get<double>();
        if (p.contains("beta"))   params_.beta   = p["beta"].get<double>();
        if (p.contains("gamma"))  params_.gamma  = p["gamma"].get<double>();
        if (p.contains("period")) params_.period = p["period"].get<int>();
    }

    // Core forecasting state
    if (s.contains("level"))    level_ = s["level"].get<double>();
    if (s.contains("trend"))    trend_ = s["trend"].get<double>();
    if (s.contains("fitted"))   fitted_= s["fitted"].get<bool>();

    if (s.contains("seasonals"))
        seasonals_ = s["seasonals"].get<std::vector<double>>();

    // Error-metric vectors — allow empty (metrics will report 0.0 if absent)
    if (s.contains("fitted_values"))
        fitted_values_ = s["fitted_values"].get<std::vector<double>>();
    if (s.contains("residuals"))
        residuals_ = s["residuals"].get<std::vector<double>>();
}

} // namespace xls20::model
