#pragma once

#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>

namespace xls20::model {

// ── Parameters ────────────────────────────────────────────────────────────────

/**
 * @brief Smoothing hyper-parameters for the Holt-Winters model.
 *
 * All three smoothing factors must be in (0, 1).
 * `period` is the number of time steps per seasonal cycle; e.g. 24 for
 * hourly buckets with a daily seasonal pattern.
 */
struct HoltWintersParams {
    double alpha{0.3};  ///< Level smoothing  (0 < α < 1)
    double beta{0.1};   ///< Trend smoothing  (0 < β < 1)
    double gamma{0.2};  ///< Seasonal smoothing (0 < γ < 1)
    int    period{24};  ///< Seasonal period length
};

// ── HoltWinters ───────────────────────────────────────────────────────────────

/**
 * @brief Additive Holt-Winters triple exponential smoothing.
 *
 * Suitable for time series that exhibit both a trend and a repeating
 * seasonal pattern.  The additive form is preferred over multiplicative
 * when the seasonal variation is roughly constant in absolute terms —
 * i.e. the amplitude of price swings does not grow proportionally with
 * the level, which is typical in NFT markets over short horizons.
 *
 * Update equations (t = current step, m = period):
 *   Lₜ  = α(yₜ − Sₜ₋ₘ)    + (1−α)(Lₜ₋₁ + Bₜ₋₁)
 *   Bₜ  = β(Lₜ − Lₜ₋₁)    + (1−β)Bₜ₋₁
 *   Sₜ  = γ(yₜ − Lₜ₋₁ − Bₜ₋₁) + (1−γ)Sₜ₋ₘ
 *   F̂ₜ₊ₕ = Lₜ + h·Bₜ + Sₜ₊ₕ₋ₘ
 *
 * Initialisation follows the classical Hyndman et al. decomposition:
 *   L₀ = average of first period; B₀ = per-step trend across first two periods;
 *   S[i] = yᵢ − L₀   for i in [0, m).
 *
 * The series must contain at least 2 × period observations.
 *
 * Phase 3 additions:
 *   export_state() / import_state() provide full bidirectional JSON
 *   serialization of every internal field (level, trend, seasonals,
 *   fitted_values, residuals, fitted flag, and params).  This lets the
 *   registry checkpoint and restore the forecaster without re-running fit().
 *
 * Not thread-safe.
 */
class HoltWinters {
public:
    explicit HoltWinters(HoltWintersParams params = {});

    /**
     * @brief Fit the model on a historical close-price series.
     * @throws std::invalid_argument if series.size() < 2 * params.period.
     */
    void fit(const std::vector<double>& series);

    // ── Phase 4: Adaptive Volatility Bands ───────────────────────────────────

    /**
     * @brief Structured forecast payload with point prediction and
     *        symmetric confidence bands.
     *
     * Bands are computed as:
     *   upper[h] = point_forecast[h] + z * rolling_rmse
     *   lower[h] = point_forecast[h] - z * rolling_rmse
     *
     * where z = 1.96 gives a 95% Gaussian confidence interval.
     * `rolling_rmse` is the RMSE from the most recent in-sample fit.
     */
    struct ForecastEnvelope {
        std::vector<double> point_forecast; ///< h-step-ahead point estimates
        std::vector<double> upper_band;     ///< Upper confidence boundary
        std::vector<double> lower_band;     ///< Lower confidence boundary
        double              band_width{0.0};///< z * rmse (constant across horizon)
    };

    /**
     * @brief Forecast the next `h` steps and return point + confidence bands.
     *
     * @param h  Horizon length (must be > 0).
     * @param z  Multiplier for the rolling RMSE (default 1.96 → 95% CI).
     * @throws std::runtime_error if fit() has not been called.
     */
    [[nodiscard]]
    ForecastEnvelope forecast_envelope(int h, double z = 1.96) const;

    /**
     * @brief Forecast the next `h` time steps beyond the fitted series.
     * @throws std::runtime_error if fit() has not been called successfully.
     */
    [[nodiscard]]
    std::vector<double> forecast(int h) const;

    /// Mean Absolute Error across the in-sample fitted values.
    [[nodiscard]] double mae()  const;

    /// Root Mean Squared Error across the in-sample fitted values.
    [[nodiscard]] double rmse() const;

    [[nodiscard]] bool is_fitted() const noexcept { return fitted_; }

    [[nodiscard]] double level()  const noexcept { return level_; }
    [[nodiscard]] double trend()  const noexcept { return trend_; }
    [[nodiscard]] const std::vector<double>& seasonals() const noexcept { return seasonals_; }

    // ── Phase 3: State serialization ──────────────────────────────────────────

    /**
     * @brief Export the complete model state to a JSON object.
     *
     * Captures params (α/β/γ/period), level, trend, all seasonal indices,
     * every fitted value, every residual, and the fitted flag.  Importing
     * this snapshot restores a bit-identical forecasting state without
     * needing to re-run fit().
     */
    [[nodiscard]] nlohmann::json export_state() const;

    /**
     * @brief Restore the model state from a previously exported JSON snapshot.
     *
     * Fields that are missing from the snapshot are left at their current
     * values, so partial updates are safe.  After a successful import the
     * model's is_fitted() state matches what was serialized.
     *
     * @throws std::invalid_argument if critical fields are malformed.
     */
    void import_state(const nlohmann::json& state);

private:
    /// Set initial level, trend, and seasonal indices from the first two full periods.
    void initialise(const std::vector<double>& series);

    HoltWintersParams    params_;
    double               level_{0.0};
    double               trend_{0.0};
    std::vector<double>  seasonals_;      ///< Length = params_.period
    std::vector<double>  fitted_values_;  ///< One entry per observation
    std::vector<double>  residuals_;      ///< yₜ − ŷₜ
    bool                 fitted_{false};
};

} // namespace xls20::model
