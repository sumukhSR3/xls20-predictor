#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// Phase 2: boost::asio::signal_set replaces std::signal() / std::atomic flag.
#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp>
#include <nlohmann/json.hpp>

#include "model/calibration.hpp"
#include "network/http_client.hpp"          // Phase 5: async HTTP GET/POST
#include "network/webhook_dispatcher.hpp"   // Phase 5: outbound alert engine
#include "network/ws_client.hpp"
#include "output/forecast_printer.hpp"
#include "parser/tx_filter.hpp"
#include "registry/collection_registry.hpp"

// Phase 3: grid search optimizer

// ── Config helpers
// ────────────────────────────────────────────────────────────
struct WSConfig {
  std::string host = "xrplcluster.com";
  std::string port = "443";
  std::string path = "/";
  int reconnect_ms = 2'000;
  int max_reconnect_ms = 64'000;
};

static WSConfig load_ws_config(const std::string &config_path) {
  WSConfig cfg;
  std::ifstream file(config_path);
  if (!file)
    return cfg;
  nlohmann::json doc;
  try {
    file >> doc;
  } catch (...) {
    return cfg;
  }
  if (doc.contains("websocket")) {
    const auto &ws = doc["websocket"];
    if (ws.contains("host"))
      cfg.host = ws["host"].get<std::string>();
    if (ws.contains("port"))
      cfg.port = ws["port"].get<std::string>();
    if (ws.contains("path"))
      cfg.path = ws["path"].get<std::string>();
    if (ws.contains("reconnect_delay_ms"))
      cfg.reconnect_ms = ws["reconnect_delay_ms"].get<int>();
    if (ws.contains("max_reconnect_delay_ms"))
      cfg.max_reconnect_ms = ws["max_reconnect_delay_ms"].get<int>();
  }
  return cfg;
}

static xls20::output::ForecastPrinter::Options
load_output_config(const std::string &config_path) {
  xls20::output::ForecastPrinter::Options opts;
  std::ifstream file(config_path);
  if (!file)
    return opts;
  nlohmann::json doc;
  try {
    file >> doc;
  } catch (...) {
    return opts;
  }
  if (doc.contains("output")) {
    const auto &o = doc["output"];
    if (o.contains("sparkline_bars"))
      opts.sparkline_bars = o["sparkline_bars"].get<int>();
    if (o.contains("show_ansi_colors"))
      opts.use_color = o["show_ansi_colors"].get<bool>();
  }
  if (doc.contains("forecasting") &&
      doc["forecasting"].contains("forecast_horizon"))
    opts.forecast_horizon = doc["forecasting"]["forecast_horizon"].get<int>();
  return opts;
}

static int load_refresh_interval(const std::string &config_path) {
  std::ifstream file(config_path);
  if (!file)
    return 60;
  nlohmann::json doc;
  try {
    file >> doc;
  } catch (...) {
    return 60;
  }
  if (doc.contains("output") &&
      doc["output"].contains("refresh_interval_seconds"))
    return doc["output"]["refresh_interval_seconds"].get<int>();
  return 60;
}

// ── Phase 5 config helper ─────────────────────────────────────────────────

struct Phase5Config {
  std::string webhook_url;       ///< Empty → disabled
  std::string api_baseline_url;  ///< Empty → no cold-start fetch
  int         baseline_timeout_s{10};
};

static Phase5Config load_phase5_config(const std::string& config_path) {
  Phase5Config cfg;
  std::ifstream file(config_path);
  if (!file) return cfg;
  nlohmann::json doc;
  try { file >> doc; } catch (...) { return cfg; }
  if (!doc.contains("phase5")) return cfg;
  const auto& p = doc["phase5"];
  if (p.contains("webhook_url") && p["webhook_url"].is_string())
    cfg.webhook_url = p["webhook_url"].get<std::string>();
  if (p.contains("api_baseline_url") && p["api_baseline_url"].is_string())
    cfg.api_baseline_url = p["api_baseline_url"].get<std::string>();
  if (p.contains("baseline_timeout_s"))
    cfg.baseline_timeout_s = p["baseline_timeout_s"].get<int>();
  return cfg;
}

// ── mock_run
// ──────────────────────────────────────────────────────────────────
/**
 * @brief Self-contained offline validation (--mock flag).
 *
 * Feeds three 48-bucket synthetic price series (↑ up / → flat / ↓ down)
 * through Holt-Winters and renders the full ANSI table without any network.
 */
static int mock_run() {
  using namespace xls20;
  using namespace xls20::model;
  using namespace xls20::output;

  std::cout << "\n[Mock] Running offline validation — no network required.\n\n";

  HoltWintersParams hw{0.3, 0.1, 0.2, 24};
  const int horizon = 24;

  auto make_result = [&](const std::string &name, const std::string &key,
                         std::vector<double> series,
                         double current) -> ForecastResult {
    HoltWinters model(hw);
    try {
      model.fit(series);
    } catch (const std::exception &e) {
      std::cerr << "[Mock] HW fit error for " << name << ": " << e.what()
                << '\n';
    }
    ForecastResult fr;
    fr.collection_name = name;
    fr.collection_key = key;
    fr.current_price_xrp = current;
    fr.total_trades = series.size() * 3;
    fr.bucket_count = series.size();
    fr.mae = model.is_fitted() ? model.mae() : 0.0;
    fr.rmse = model.is_fitted() ? model.rmse() : 0.0;
    fr.predictions =
        model.is_fitted()
            ? model.forecast(horizon)
            : std::vector<double>(static_cast<size_t>(horizon), current);
    if (!fr.predictions.empty()) {
      double diff = fr.predictions.front() - current;
      fr.trend_direction = (diff > 0.005 * current)    ? 1
                           : (diff < -0.005 * current) ? -1
                                                       : 0;
    }
    return fr;
  };

  // XPunks: uptrend + daily seasonality → ↑
  std::vector<double> xpunks;
  xpunks.reserve(48);
  for (int i = 0; i < 48; ++i) {
    xpunks.push_back(std::max(0.01, 5.0 + 0.065 * i +
                                        0.6 * std::sin(2.0 * M_PI * i / 24.0) +
                                        0.08 * ((i * 17 + 3) % 7 - 3)));
  }
  // Bored Apes: flat → →
  std::vector<double> bored;
  bored.reserve(48);
  for (int i = 0; i < 48; ++i) {
    bored.push_back(std::max(0.01, 12.0 +
                                       0.25 * std::sin(2.0 * M_PI * i / 7.0) +
                                       0.12 * ((i * 31 + 5) % 5 - 2)));
  }
  // xSPECTAR: declining → ↓
  std::vector<double> spectar;
  spectar.reserve(48);
  for (int i = 0; i < 48; ++i) {
    spectar.push_back(std::max(0.01, 3.5 - 0.03 * i +
                                         0.2 * std::cos(2.0 * M_PI * i / 24.0) +
                                         0.06 * ((i * 13 + 7) % 5 - 2)));
  }

  std::vector<ForecastResult> results;
  results.push_back(
      make_result("XPunks", "XPUNKISSUER:0", xpunks, xpunks.back()));
  results.push_back(
      make_result("Bored Apes XRPL", "BOREDAPEISSUER:1", bored, bored.back()));
  results.push_back(
      make_result("xSPECTAR", "XSPECTRUMISSUER:0", spectar, spectar.back()));

  ForecastPrinter::Options opts;
  opts.sparkline_bars = 24;
  opts.use_color = true;
  opts.forecast_horizon = horizon;
  ForecastPrinter(opts).print(results);

  std::cout << "\n[Mock] Validation complete.  "
               "ANSI table, sparklines, and trend arrows verified.\n"
            << "[Mock] Re-run without --mock to connect to "
               "wss://xrplcluster.com live.\n\n";
  return EXIT_SUCCESS;
}

// ── backtest_run
// ──────────────────────────────────────────────────────────────
/**
 * @brief Phase 3: Offline hyperparameter calibration (--backtest <file>).
 *
 * Accepts either:
 *   - Format v2  (data/historical_cache.json written by the live agent):
 *       doc["collections"][key] = { "buckets": [...], "hw_state": {...}, ... }
 *   - Format v1  (legacy bare-array cache):
 *       doc["collections"][key] = [ { "close": ..., ... }, ... ]
 *   - Simple series format:
 *       doc = { "period": 24, "series": [{"name":"X","closes":[...]},...] }
 *
 * For each collection / series, an exhaustive (α,β,γ) grid search is run via
 * xls20::model::grid_search().  The optimal parameters are printed in a table
 * and can be pasted directly into the "forecasting" section of
 * config/collections.json to improve live-prediction accuracy.
 *
 * Progress is reported per α-slice so the operator can see the search advance
 * in real time without waiting for the full 6 859-combination sweep.
 */
static int backtest_run(const std::string &data_path) {
  using namespace xls20::model;
  using json = nlohmann::json;

  // ── 1. Load data file ─────────────────────────────────────────────────────
  std::ifstream file(data_path);
  if (!file) {
    std::cerr << "[Backtest] Cannot open: " << data_path << '\n';
    return EXIT_FAILURE;
  }
  json doc;
  try {
    file >> doc;
  } catch (const std::exception &e) {
    std::cerr << "[Backtest] JSON parse error: " << e.what() << '\n';
    return EXIT_FAILURE;
  }

  // Unicode box-drawing chars are multi-byte UTF-8; std::string(n, char) only
  // accepts single bytes.  Build the separator by appending the UTF-8 string.
  auto rep = [](const char *s, int n) {
    std::string r;
    r.reserve(static_cast<size_t>(n) * 3);
    for (int i = 0; i < n; ++i)
      r += s;
    return r;
  };
  const std::string sep_thick = rep("═", 89);

  // ── 2. Extract (name, close-price vector, period) triples ─────────────────
  struct SeriesEntry {
    std::string name;
    std::vector<double> closes;
    int period{24};
  };
  std::vector<SeriesEntry> all_series;

  const int global_period = doc.value("period", 24);

  if (doc.contains("collections") && doc["collections"].is_object()) {
    // Format A: cache file (v1 or v2)
    for (const auto &[key, val] : doc["collections"].items()) {
      SeriesEntry e;
      e.name = key;
      e.period = global_period;

      const json *buckets_ptr = nullptr;
      if (val.is_array()) {
        // v1 — bare array of bucket objects
        buckets_ptr = &val;
      } else if (val.is_object() && val.contains("buckets") &&
                 val["buckets"].is_array()) {
        // v2 — structured entry
        buckets_ptr = &val["buckets"];
      }

      if (buckets_ptr) {
        for (const auto &b : *buckets_ptr) {
          if (b.contains("close"))
            e.closes.push_back(b["close"].get<double>());
        }
      }
      if (!e.closes.empty())
        all_series.push_back(std::move(e));
    }
  } else if (doc.contains("series") && doc["series"].is_array()) {
    // Format B: simple series array
    for (const auto &s : doc["series"]) {
      SeriesEntry e;
      e.name = s.value("name", "Unknown");
      e.period = s.value("period", global_period);
      if (s.contains("closes"))
        e.closes = s["closes"].get<std::vector<double>>();
      if (!e.closes.empty())
        all_series.push_back(std::move(e));
    }
  }

  if (all_series.empty()) {
    std::cerr << "[Backtest] No valid series found in " << data_path << ".\n"
              << "           Expected cache format (v1/v2) or:\n"
              << "           { \"period\": 24, \"series\": "
                 "[{\"name\":\"X\",\"closes\":[...]},...] }\n";
    return EXIT_FAILURE;
  }

  // ── 3. Print header ───────────────────────────────────────────────────────

  std::cout << '\n'
            << "╔" << sep_thick << "╗\n"
            << "║  Phase 3 · Holt-Winters Hyperparameter Grid Search"
               "  (α,β,γ ∈ [0.05, 0.95], step 0.05)  ║\n"
            << "╠" << sep_thick << "╣\n"
            << std::left << "║ " << std::setw(22) << "Collection"
            << "│" << std::right << std::setw(8) << " Alpha "
            << "│" << std::setw(8) << " Beta  "
            << "│" << std::setw(8) << " Gamma "
            << "│" << std::setw(10) << "   MAE    "
            << "│" << std::setw(10) << "   RMSE   "
            << "│" << std::setw(7) << "  Pts  "
            << "│" << std::setw(9) << " Combos "
            << " ║\n"
            << "╠" << sep_thick << "╣\n";

  // ── 4. Run grid search for each series ────────────────────────────────────
  int series_idx = 0;
  for (const auto &entry : all_series) {
    ++series_idx;
    const int min_pts = 2 * entry.period;

    if (static_cast<int>(entry.closes.size()) < min_pts) {
      std::cout << "║ " << std::left << std::setw(22)
                << entry.name.substr(0, 22) << "│  Insufficient data — need ≥ "
                << min_pts << " pts, have " << entry.closes.size()
                << "          ║\n";
      continue;
    }

    // Live progress bar — prints a dot per α-slice as the search advances.
    std::cout << "║  Calibrating [" << series_idx << "/" << all_series.size()
              << "] " << entry.name.substr(0, 30) << " … " << std::flush;

    int last_pct = -1;
    auto result = grid_search_verbose(
        entry.closes,
        [&](int cur, int total) {
          int pct = (total > 0) ? (cur * 100 / total) : 100;
          if (pct != last_pct && pct % 10 == 0) {
            std::cout << pct << "% " << std::flush;
            last_pct = pct;
          }
        },
        entry.period);

    std::cout << '\n';

    std::cout << std::fixed << std::setprecision(4) << "║ " << std::left
              << std::setw(22) << entry.name.substr(0, 22) << "│" << std::right
              << std::setw(7) << result.params.alpha << " "
              << "│" << std::setw(7) << result.params.beta << " "
              << "│" << std::setw(7) << result.params.gamma << " "
              << "│" << std::setw(9) << result.mae << " "
              << "│" << std::setw(9) << result.rmse << " "
              << "│" << std::setw(6) << entry.closes.size() << " "
              << "│" << std::setw(7) << result.n_evaluated << "  ║\n";
  }

  // ── 5. Footer with actionable config snippet ───────────────────────────────
  std::cout
      << "╚" << sep_thick << "╝\n\n"
      << "[Backtest] Completed " << all_series.size() << " series.\n\n"
      << "  To apply the optimal parameters, update config/collections.json:\n"
      << "  {\n"
      << "    \"forecasting\": {\n"
      << "      \"alpha\":  <best_alpha>,\n"
      << "      \"beta\":   <best_beta>,\n"
      << "      \"gamma\":  <best_gamma>\n"
      << "    }\n"
      << "  }\n"
      << "  Then restart the live agent — it will re-fit on the warm-started\n"
      << "  historical cache using the calibrated parameters.\n\n";

  return EXIT_SUCCESS;
}

// ── main
// ──────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
  // ── Argument parsing ──────────────────────────────────────────────────────
  std::string config_path = "config/collections.json";
  std::string backtest_path = "";
  bool mock_mode = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--mock") {
      mock_mode = true;
    } else if (arg == "--backtest" && i + 1 < argc) {
      backtest_path = argv[++i];
    } else {
      config_path = arg;
    }
  }

  std::cout << "╔═══════════════════════════════════════════╗\n"
            << "║  XLS-20 NFT Price Predictor Agent         ║\n"
            << "║  v1.3.0  •  Phase 5: Outbound Alerting    ║\n"
            << "╚═══════════════════════════════════════════╝\n\n";

  // ── Phase 3: Backtest / calibration track ────────────────────────────────
  if (!backtest_path.empty()) {
    std::cout
        << "[Backtest] Grid-search calibration on: " << backtest_path << "\n"
        << "[Backtest] Sweep: α,β,γ ∈ [0.05,0.95] step 0.05 → 6 859 combos\n\n";
    return backtest_run(backtest_path);
  }

  // ── Mock mode: offline rendering validation ───────────────────────────────
  if (mock_mode)
    return mock_run();

  // ── ASIO IO context ───────────────────────────────────────────────────────
  boost::asio::io_context ioc;
  auto work_guard = boost::asio::make_work_guard(ioc);

  // ── Registry (load config + warm-start cache) ─────────────────────────────
  xls20::CollectionRegistry::Config reg_cfg;
  reg_cfg.config_path = config_path;
  reg_cfg.cache_path = "data/historical_cache.json";

  auto registry = std::make_shared<xls20::CollectionRegistry>(reg_cfg);
  if (!registry->load_collections()) {
    std::cerr << "[Main] Failed to load any collections.  Check " << config_path
              << "\n";
    return EXIT_FAILURE;
  }
  // Warm-start: load_cache() now restores full HW mathematical state
  // (level, trend, seasonals, residuals) via import_state() — no re-fit needed.
  registry->load_cache();

  // ── Phase 5: webhook dispatcher ──────────────────────────────────────────
  Phase5Config p5_cfg = load_phase5_config(config_path);

  if (!p5_cfg.webhook_url.empty()) {
    auto dispatcher = std::make_shared<xls20::net::WebhookDispatcher>(
        ioc, p5_cfg.webhook_url);
    registry->set_webhook_dispatcher(dispatcher);
    std::cout << "[Phase5] Webhook alerts enabled → "
              << p5_cfg.webhook_url << '\n';
  } else {
    std::cout << "[Phase5] Webhook URL not configured — alerts disabled.\n";
  }

  // ── Output printer ────────────────────────────────────────────────────────
  auto printer_opts = load_output_config(config_path);
  xls20::output::ForecastPrinter printer(printer_opts);
  const int refresh_interval = load_refresh_interval(config_path);

  // ── Phase 2: ASIO signal_set for SIGINT / SIGTERM ────────────────────────
  boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
  std::atomic<bool> printer_active{true};

  // ── Phase 5: async historical baseline fetch ─────────────────────────────
  //
  // Fires a non-blocking HTTP GET on `ioc` *before* ioc.run() is called,
  // queuing the operation as a pending async chain.  When ioc.run() starts it
  // processes both the WebSocket connect and this baseline fetch concurrently
  // on the same thread — the single-threaded event-loop design is preserved.
  if (!p5_cfg.api_baseline_url.empty()) {
    std::cout << "[Phase5] Scheduling historical baseline fetch from: "
              << p5_cfg.api_baseline_url << '\n';

    const uint64_t bucket_dur = reg_cfg.bucket_dur_s;

    xls20::net::async_http_get(
        ioc,
        p5_cfg.api_baseline_url,
        [registry, bucket_dur]
        (bool ok, unsigned status, std::string body) {

          if (!ok || status != 200) {
            std::cerr << "[Phase5] Baseline fetch failed (HTTP "
                      << status << ") — cold-start on live data only.\n";
            return;
          }

          nlohmann::json doc;
          try { doc = nlohmann::json::parse(body); }
          catch (const std::exception& e) {
            std::cerr << "[Phase5] Baseline JSON parse error: "
                      << e.what() << '\n';
            return;
          }

          // Base timestamp = now (represents the most recent bucket received).
          const uint64_t now_s = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count());

          int total_seeded = 0;

          // ── Format A: cache v1/v2 (collections dict) ─────────────────────
          if (doc.contains("collections") &&
              doc["collections"].is_object()) {
            for (const auto& [key, val] : doc["collections"].items()) {
              std::vector<double> closes;
              const nlohmann::json* bp = nullptr;
              if (val.is_array())
                bp = &val;
              else if (val.is_object() && val.contains("buckets") &&
                       val["buckets"].is_array())
                bp = &val["buckets"];
              if (bp) {
                for (const auto& b : *bp)
                  if (b.contains("close"))
                    closes.push_back(b["close"].get<double>());
              }
              if (!closes.empty())
                total_seeded += registry->ingest_historical(
                    key, closes, now_s, bucket_dur);
            }
          }
          // ── Format B: simple series array ──────────────────────────────
          else if (doc.contains("series") && doc["series"].is_array()) {
            for (const auto& s : doc["series"]) {
              // Accept either "key" (canonical) or fall back to "name".
              std::string key = s.value("key", s.value("name", std::string{}));
              std::vector<double> closes;
              if (s.contains("closes") && s["closes"].is_array())
                closes = s["closes"].get<std::vector<double>>();
              if (!key.empty() && !closes.empty())
                total_seeded += registry->ingest_historical(
                    key, closes, now_s, bucket_dur);
            }
          }

          std::cout << "[Phase5] Historical baseline complete: seeded "
                    << total_seeded << " total buckets.\n";
        });
  } else {
    std::cout << "[Phase5] No baseline URL configured — "
                 "starting fresh or from disk cache.\n";
  }

  // ── WebSocket client ──────────────────────────────────────────────────────
  WSConfig ws_cfg = load_ws_config(config_path);
  xls20::parser::TxFilter filter;

  std::cout << "[Main] Connecting to wss://" << ws_cfg.host << ws_cfg.path
            << " …\n";

  auto ws_client = std::make_shared<xls20::net::WSClient>(
      ioc, ws_cfg.host, ws_cfg.port, ws_cfg.path,
      [&](std::string_view msg) {
        auto sale = filter.filter(msg);
        if (!sale)
          return;
        // ingest() now auto-checkpoints to disk on every bucket close.
        if (registry->ingest(*sale)) {
          std::cout << "[Ingest] Sale " << sale->nft_id.substr(0, 12) << "…  "
                    << sale->price_xrp << " XRP"
                    << "  (" << sale->collection_key << ")\n";
        }
      },
      ws_cfg.reconnect_ms, ws_cfg.max_reconnect_ms
      /* ping_interval_s defaults to 30 */
  );
  ws_client->connect();

  // ── Signal handler (runs on ASIO thread) ──────────────────────────────────
  signals.async_wait([&](const boost::system::error_code &ec, int signo) {
    if (ec)
      return;

    std::cout << "\n[Main] Signal " << signo
              << " received — initiating clean shutdown…\n";

    printer_active.store(false, std::memory_order_release);
    ws_client->disconnect();

    // Final cache flush: captures any partial-bucket state and ensures
    // the last checkpoint is up to date before process exit.
    std::cout << "[Main] Saving historical cache (final flush)…\n";
    if (registry->save_cache())
      std::cout << "[Main] Cache written to " << reg_cfg.cache_path << "\n";
    else
      std::cerr << "[Main] WARNING: final cache write failed.\n";

    work_guard.reset();
  });

  // ── Hot-reload timer (runs on ASIO thread every 30 s) ─────────────────────
  auto reload_timer = std::make_shared<boost::asio::steady_timer>(ioc);
  std::function<void()> arm_reload_timer = [&, reload_timer]() {
    reload_timer->expires_after(std::chrono::seconds(30));
    reload_timer->async_wait(
        [&, reload_timer](const boost::system::error_code &ec) {
          if (ec)
            return;
          registry->check_config_reload();
          arm_reload_timer();
        });
  };
  arm_reload_timer();

  // ── Printer thread ────────────────────────────────────────────────────────
  std::thread printer_thread([&]() {
    while (printer_active.load(std::memory_order_acquire)) {
      printer.print(registry->get_forecasts());
      for (int i = 0; i < refresh_interval * 10; ++i) {
        if (!printer_active.load(std::memory_order_acquire))
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
    std::cout << "[Printer] Table output stopped.\n";
  });

  // ── Event loop ────────────────────────────────────────────────────────────
  ioc.run();

  if (printer_thread.joinable())
    printer_thread.join();
  std::cout << "[Main] Goodbye.\n";
  return EXIT_SUCCESS;
}
