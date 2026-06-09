# XLS-20 NFT Price Predictor Agent

A highly optimized C++20 quantitative trading agent built for the XRP Ledger (XRPL). It monitors live XLS-20 NFT trades via WebSockets, calculates real-time price predictions using the Holt-Winters exponential smoothing model, and provides adaptive volatility bands to generate asynchronous `BUY` and `SELL` signals.

## Features

- **Live WebSocket Data Ingestion:** Hooks directly into XRPL wss endpoints (like `xrplcluster.com`) to stream real-time NFT transactions.
- **Holt-Winters Forecasting Model:** Utilizes triple exponential smoothing (level, trend, and seasonality) to forecast future NFT price action based on historical and real-time bucketed data.
- **Adaptive Volatility Bands:** Computes upper and lower 95% confidence intervals dynamically using a rolling Root Mean Square Error (RMSE). Breaks below the lower band generate `BUY` signals; breaks above generate `SELL` signals.
- **Asynchronous Webhook Alerts:** Built on Boost.Asio and Boost.Beast, allowing non-blocking HTTP POST payloads to immediately notify your external systems of signal transitions.
- **State Durability & Cold-Start Prevention:** Caches market state locally (`historical_cache.json`) and supports fetching historical baseline data on startup to bypass cold starts.
- **Terminal UI Dashboard:** Renders an elegant ANSI-colored dashboard in the terminal showing live prices, trends, forecasts, and actionable trading signals.
- **Production Hardened Docker Build:** Multi-stage Dockerfile that provides an ultra-minimal runtime layer (<80 MB) with no root access and built-in health checks.

## Project Structure

```
├── CMakeLists.txt              # CMake build configuration
├── Dockerfile                  # Multi-stage containerization
├── config/
│   └── collections.json        # Main agent configuration
├── data/
│   └── historical_cache.json   # Persistent state storage
├── scripts/
│   └── test_runner.sh          # Integration test suite
└── src/
    ├── main.cpp                # Core event loop and bootstrapping
    ├── model/                  # Holt-Winters math and price aggregations
    ├── network/                # Boost.Asio WebSocket and HTTP clients
    ├── output/                 # Terminal UI components
    ├── parser/                 # XRPL transaction decoding logic
    └── registry/               # Memory and state management for tracked NFTs
```

## Prerequisites

- **CMake** 3.20+
- **GCC-13+ / Clang 16+** (Must support C++20)
- **Boost** (1.83.0+ recommended for `system` and `asio`)
- **OpenSSL**
- **nlohmann_json** (`nlohmann-json3-dev`)

## Building Locally

To build the executable manually on your host system:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel $(sysctl -n hw.ncpu 2>/dev/null || nproc)
```

The resulting binary will be output as `./build/XLS20PredictorAgent`.

## Configuration

Settings are managed via `config/collections.json`. Here you can configure forecasting horizons, bucket durations, target collections, and webhook URLs.

```json
{
  "websocket": { "host": "xrplcluster.com", "port": "443", "path": "/" },
  "forecasting": { "bucket_duration_seconds": 3600, "forecast_horizon": 24 },
  "output": { "refresh_interval_seconds": 60, "show_ansi_colors": true },
  "phase5": {
    "webhook_url": "http://your-alerting-server.com/webhook",
    "api_baseline_url": "http://your-historical-server.com/baseline"
  },
  "collections": [
    {
      "name": "XPunks",
      "issuer": "rXPunKsExampleIssuerAddressHere1",
      "taxon": 0
    }
  ]
}
```

## Running the Agent

Run the live agent tracking the configured NFT collections:
```bash
./build/XLS20PredictorAgent config/collections.json
```

**Grid-Search Calibration:**
You can calibrate the optimal `alpha`, `beta`, and `gamma` parameters using historical backtesting:
```bash
./build/XLS20PredictorAgent --backtest data/historical_cache.json
```

**Smoke Test Mode:**
Run offline validation without connecting to the network:
```bash
./build/XLS20PredictorAgent --mock
```

## Docker Deployment

Build the container image:
```bash
docker build -t xls20-agent:latest .
```

Run as a daemon (mapping your local config and data folders):
```bash
docker run -d \
  --name xls20-live \
  --restart unless-stopped \
  -v "$(pwd)/config:/config:ro" \
  -v "$(pwd)/data:/data" \
  xls20-agent:latest /config/collections.json
```

## Testing

Run the included integration test runner which validates directory structures, config schemas, runs the mock health checks, and verifies the binary architecture:

```bash
chmod +x scripts/test_runner.sh
./scripts/test_runner.sh
```


