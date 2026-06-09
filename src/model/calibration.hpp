#pragma once

/**
 * @file calibration.hpp
 * @brief Phase 3: Hyperparameter calibration via exhaustive grid search.
 *
 * This header is intentionally self-contained and header-only.  It depends
 * only on the model tier (holt_winters.hpp) and the C++20 standard library.
 * No additional CMake source registration is needed.
 *
 * Grid dimensions (defaults):
 *   α, β, γ  each swept 0.05 → 0.95 in 0.05 steps  →  19 values per axis
 *   Total combinations: 19³ = 6 859
 *
 * Each combination runs a full HoltWinters::fit() on the supplied series and
 * scores it by in-sample MAE; ties are broken by RMSE.  The search is purely
 * sequential; for a 48-point series on modern hardware the wall-clock time is
 * well under 100 ms even for the full 6 859-combination sweep.
 *
 * Usage example:
 * @code
 *   #include "model/calibration.hpp"
 *
 *   // period = 24 (hourly buckets, daily cycle)
 *   auto result = xls20::model::grid_search(close_prices, 24);
 *   std::cout << "Best α=" << result.params.alpha
 *             << " β=" << result.params.beta
 *             << " γ=" << result.params.gamma
 *             << " MAE=" << result.mae << '\n';
 * @endcode
 */

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "model/holt_winters.hpp"

namespace xls20::model {

// ── CalibrationResult ─────────────────────────────────────────────────────────

/// Returned by grid_search() with the optimal parameter set found.
struct CalibrationResult {
    HoltWintersParams params;                    ///< Best α / β / γ / period
    double            mae  {std::numeric_limits<double>::max()};
    double            rmse {std::numeric_limits<double>::max()};
    int               n_evaluated{0};            ///< Total (α,β,γ) combinations tested
    int               n_skipped  {0};            ///< Skipped (fit() threw — too few pts)
};

// ── grid_search ───────────────────────────────────────────────────────────────

/**
 * @brief Exhaustive grid search over the (α, β, γ) smoothing parameter space.
 *
 * Each parameter is swept from `lo` to `hi` (inclusive) in steps of `step`.
 * Integer loop indices are used internally to avoid floating-point
 * accumulation error across the step sequence.
 *
 * Selection criterion: lowest in-sample MAE; ties broken by RMSE.
 *
 * @param series  Historical close-price series.  Must contain ≥ 2 × period pts.
 * @param period  Seasonal period (default 24 — hourly buckets, daily cycle).
 * @param lo      Lower bound for each smoothing parameter, inclusive.
 * @param hi      Upper bound for each smoothing parameter, inclusive.
 * @param step    Grid resolution (default 0.05 → 19 values per axis).
 * @return        Best parameter combination and its in-sample error metrics.
 *                If the series is too short for *any* combination the returned
 *                result has mae/rmse == numeric_limits<double>::max() and the
 *                caller should check n_evaluated == 0.
 */
[[nodiscard]]
inline CalibrationResult grid_search(
    const std::vector<double>& series,
    int    period = 24,
    double lo     = 0.05,
    double hi     = 0.95,
    double step   = 0.05)
{
    CalibrationResult best;
    best.params.period = period;

    // Number of steps per axis: round-trip to avoid fp drift
    const int n_steps = static_cast<int>(std::round((hi - lo) / step)) + 1;

    for (int ia = 0; ia < n_steps; ++ia) {
        const double alpha = lo + ia * step;

        for (int ib = 0; ib < n_steps; ++ib) {
            const double beta = lo + ib * step;

            for (int ig = 0; ig < n_steps; ++ig) {
                const double gamma = lo + ig * step;

                HoltWintersParams p{alpha, beta, gamma, period};
                HoltWinters       hw(p);

                try {
                    hw.fit(series);
                } catch (...) {
                    ++best.n_evaluated;
                    ++best.n_skipped;
                    continue;   // series too short for this period — skip
                }

                const double candidate_mae  = hw.mae();
                const double candidate_rmse = hw.rmse();

                const bool better_mae  = candidate_mae < best.mae;
                const bool tie_mae     = (candidate_mae == best.mae);
                const bool better_rmse = candidate_rmse < best.rmse;

                if (better_mae || (tie_mae && better_rmse)) {
                    best.mae    = candidate_mae;
                    best.rmse   = candidate_rmse;
                    best.params = p;
                }

                ++best.n_evaluated;
            }
        }
    }

    return best;
}

// ── grid_search_verbose ───────────────────────────────────────────────────────

/**
 * @brief grid_search() with a live progress callback.
 *
 * The `progress` callback is invoked once per α-slice (i.e. `n_steps` times
 * in total) with the zero-based slice index and the total number of slices.
 * Useful for driving a CLI progress bar without adding any threading overhead.
 *
 * @param progress  Called as progress(current_slice, total_slices).
 */
template <typename ProgressFn>
[[nodiscard]]
inline CalibrationResult grid_search_verbose(
    const std::vector<double>& series,
    ProgressFn                 progress,
    int    period = 24,
    double lo     = 0.05,
    double hi     = 0.95,
    double step   = 0.05)
{
    CalibrationResult best;
    best.params.period = period;

    const int n_steps = static_cast<int>(std::round((hi - lo) / step)) + 1;

    for (int ia = 0; ia < n_steps; ++ia) {
        const double alpha = lo + ia * step;
        progress(ia, n_steps);   // notify caller of progress

        for (int ib = 0; ib < n_steps; ++ib) {
            const double beta = lo + ib * step;

            for (int ig = 0; ig < n_steps; ++ig) {
                const double gamma = lo + ig * step;

                HoltWintersParams p{alpha, beta, gamma, period};
                HoltWinters       hw(p);

                try {
                    hw.fit(series);
                } catch (...) {
                    ++best.n_evaluated;
                    ++best.n_skipped;
                    continue;
                }

                const double cm = hw.mae();
                const double cr = hw.rmse();

                if (cm < best.mae || (cm == best.mae && cr < best.rmse)) {
                    best.mae    = cm;
                    best.rmse   = cr;
                    best.params = p;
                }
                ++best.n_evaluated;
            }
        }
    }
    progress(n_steps, n_steps);   // 100%
    return best;
}

} // namespace xls20::model
